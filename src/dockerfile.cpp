#include "dockerfile.hpp"
#include "json.hpp"
#include "work.hpp"
#include "logger.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <cctype>
#include <cerrno>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>

namespace dockerfile {
namespace {

// ---- small string helpers ----------------------------------------------------

std::string rstrip_ws(std::string s) {
	while ( !s.empty() && ( s.back() == ' ' || s.back() == '\t' )) s.pop_back();
	return s;
}
std::string strip_ws(const std::string& s) {
	std::string::size_type a = s.find_first_not_of(" \t");
	if ( a == std::string::npos ) return "";
	std::string::size_type b = s.find_last_not_of(" \t");
	return s.substr(a, b - a + 1);
}
std::string upper(std::string s) {
	for ( char& c : s ) c = (char)toupper((unsigned char)c);
	return s;
}
std::vector<std::string> split_ws(const std::string& s) {
	std::vector<std::string> out; std::string cur;
	for ( char c : s ) {
		if ( c == ' ' || c == '\t' ) { if ( !cur.empty()) { out.push_back(cur); cur.clear(); } }
		else cur += c;
	}
	if ( !cur.empty()) out.push_back(cur);
	return out;
}
// the line after the first word + its trailing whitespace (sed 's/^\S*\s*//')
std::string after_first_word(const std::string& dl) {
	std::string::size_type ws = dl.find_first_of(" \t");
	if ( ws == std::string::npos ) return "";
	std::string::size_type a = dl.find_first_not_of(" \t", ws);
	return ( a == std::string::npos ) ? "" : dl.substr(a);
}

// Logical Dockerfile lines: strip CR, drop full-line comments (^\s*#), join
// backslash continuations, trim, drop empties. Mirrors the shell's grep+awk.
std::vector<std::string> read_logical_lines(const std::string& path, std::string& err) {
	std::ifstream f(path);
	if ( !f ) { err = "cannot read " + path; return {}; }
	std::vector<std::string> out;
	std::string raw, buf;
	while ( std::getline(f, raw)) {
		if ( !raw.empty() && raw.back() == '\r' ) raw.pop_back();
		std::string::size_type a = raw.find_first_not_of(" \t");
		if ( a != std::string::npos && raw[a] == '#' ) continue;   // full-line comment
		std::string l = buf.empty() ? raw : ( buf + " " + raw );
		buf.clear();
		std::string t = rstrip_ws(l);
		if ( !t.empty() && t.back() == '\\' ) { t.pop_back(); buf = t; continue; }   // continuation
		std::string e = strip_ws(l);
		if ( !e.empty()) out.push_back(e);
	}
	if ( !buf.empty()) { std::string e = strip_ws(buf); if ( !e.empty()) out.push_back(e); }
	return out;
}

void mkdir_p(const std::string& path) {
	std::string cur;
	for ( std::string::size_type i = 0; i < path.size(); ++i ) {
		cur += path[i];
		if ( path[i] == '/' || i + 1 == path.size()) {
			if ( !cur.empty() && cur != "/" ) mkdir(cur.c_str(), 0755);
		}
	}
}
std::string dirname_of(const std::string& p) {
	std::string::size_type s = p.find_last_of('/');
	if ( s == std::string::npos ) return ".";
	return ( s == 0 ) ? "/" : p.substr(0, s);
}

// ---- child execution with cancellation --------------------------------------

// >=0 : raw wait status; -1 : waitpid error; -2 : cancelled (group killed)
int wait_child(pid_t pid) {
	int st = 0;
	for (;;) {
		pid_t r = waitpid(pid, &st, 0);
		if ( r == pid ) return st;
		if ( r < 0 && errno == EINTR ) {
			if ( work::cancelled ) { kill(-pid, SIGKILL); waitpid(pid, &st, 0); return -2; }
			continue;
		}
		if ( r < 0 ) return -1;
	}
}

// Run `script` with /bin/sh -c inside the chroot. Child is its own process
// group so cancellation can kill the whole tree (RUN may spawn helpers).
bool run_in_chroot(const std::string& rootfs, const std::string& script, std::string& err) {
	if ( work::cancelled ) { err = "cancelled"; return false; }
	pid_t pid = fork();
	if ( pid < 0 ) { err = "fork failed"; return false; }
	if ( pid == 0 ) {
		setpgid(0, 0);
		if ( chroot(rootfs.c_str()) != 0 ) _exit(127);
		if ( chdir("/") != 0 ) _exit(127);
		execl("/bin/sh", "/bin/sh", "-c", script.c_str(), (char*)nullptr);
		_exit(127);
	}
	setpgid(pid, pid);   // race-free with the child's own setpgid
	int st = wait_child(pid);
	if ( st == -2 ) { err = "cancelled"; return false; }
	if ( st < 0 ) { err = "waitpid failed"; return false; }
	if ( WIFEXITED(st) && WEXITSTATUS(st) == 0 ) return true;
	err = "command exited non-zero";
	return false;
}

bool run_cp(const std::string& src, const std::string& dst, std::string& err) {
	if ( work::cancelled ) { err = "cancelled"; return false; }
	pid_t pid = fork();
	if ( pid < 0 ) { err = "fork failed"; return false; }
	if ( pid == 0 ) {
		setpgid(0, 0);
		execlp("cp", "cp", "-a", src.c_str(), dst.c_str(), (char*)nullptr);
		_exit(127);
	}
	setpgid(pid, pid);
	int st = wait_child(pid);
	if ( st == -2 ) { err = "cancelled"; return false; }
	if ( st < 0 ) { err = "waitpid failed"; return false; }
	if ( WIFEXITED(st) && WEXITSTATUS(st) == 0 ) return true;
	err = "cp failed";
	return false;
}

// ---- rootfs mounts for RUN (RAII: always torn down) -------------------------

struct Mounts {
	std::string rootfs;
	bool proc = false, dev = false, sys = false, ok = false;
	std::string err;

	explicit Mounts(const std::string& r) : rootfs(r) {
		mkdir(( rootfs + "/proc" ).c_str(), 0755);
		mkdir(( rootfs + "/dev"  ).c_str(), 0755);
		mkdir(( rootfs + "/sys"  ).c_str(), 0755);
		mkdir(( rootfs + "/etc"  ).c_str(), 0755);
		// clear any stale binds left by a previously crashed build (defensive)
		umount2(( rootfs + "/proc" ).c_str(), MNT_DETACH);
		umount2(( rootfs + "/dev"  ).c_str(), MNT_DETACH);
		umount2(( rootfs + "/sys"  ).c_str(), MNT_DETACH);

		if ( mount("/proc", ( rootfs + "/proc" ).c_str(), nullptr, MS_BIND, nullptr) == 0 ) proc = true;
		else { err = "cannot bind /proc into rootfs (need root)"; return; }
		if ( mount("/dev",  ( rootfs + "/dev"  ).c_str(), nullptr, MS_BIND, nullptr) == 0 ) dev = true;
		if ( mount("/sys",  ( rootfs + "/sys"  ).c_str(), nullptr, MS_BIND, nullptr) == 0 ) sys = true;

		std::ifstream i("/etc/resolv.conf", std::ios::binary);   // cp -L: host resolver
		if ( i ) { std::ofstream o(rootfs + "/etc/resolv.conf", std::ios::binary); if ( o ) o << i.rdbuf(); }
		ok = true;
	}
	~Mounts() {
		if ( sys )  umount2(( rootfs + "/sys"  ).c_str(), MNT_DETACH);
		if ( dev )  umount2(( rootfs + "/dev"  ).c_str(), MNT_DETACH);
		if ( proc ) umount2(( rootfs + "/proc" ).c_str(), MNT_DETACH);
	}
	Mounts(const Mounts&) = delete;
	Mounts& operator=(const Mounts&) = delete;
};

// set cfg[field] from a Dockerfile exec/shell-form argument
bool set_cmd_like(JSON& cfg, const char* field, const std::string& kw, const std::string& arg, std::string& err) {
	if ( !arg.empty() && arg[0] == '[' ) {                 // exec form: JSON array
		JSON a;
		try { a = JSON::parse(arg); } catch ( const std::exception& ) { err = kw + ": invalid JSON array: " + arg; return false; }
		if ( a.type() != JSON::TYPE::ARRAY ) { err = kw + ": not a JSON array: " + arg; return false; }
		cfg[field] = a;
	} else {                                               // shell form
		JSON a = JSON::Array();
		a.append(JSON(std::string("/bin/sh")));
		a.append(JSON(std::string("-c")));
		a.append(JSON(arg));
		cfg[field] = a;
	}
	return true;
}

} // anonymous namespace

std::string parse_from(const std::string& dockerfile_path, std::string& err) {
	err.clear();
	std::vector<std::string> lines = read_logical_lines(dockerfile_path, err);
	if ( !err.empty()) return "";

	std::string ref;
	int from_count = 0;
	for ( const std::string& l : lines ) {
		std::vector<std::string> tk = split_ws(l);
		if ( tk.empty() || upper(tk[0]) != "FROM" ) continue;
		if ( ++from_count > 1 ) { err = "multi-stage builds are not supported (single FROM only)"; return ""; }
		for ( std::vector<std::string>::size_type i = 1; i < tk.size(); ++i ) {
			if ( tk[i].rfind("--", 0) == 0 ) continue;          // --platform=... etc.
			if ( ref.empty()) { ref = tk[i]; continue; }
			if ( upper(tk[i]) == "AS" ) { err = "multi-stage builds are not supported (single FROM only)"; return ""; }
		}
	}
	if ( ref.empty()) { err = "no FROM instruction in " + dockerfile_path; return ""; }
	return ref;
}

bool apply(const std::string& dockerfile_path, const std::string& context_dir,
           const std::string& rootfs_dir, const std::string& config_path, std::string& err) {
	err.clear();

	// load the base image config (mutated in place by ENV/WORKDIR/USER/CMD/ENTRYPOINT)
	JSON blob;
	{
		std::ifstream f(config_path);
		if ( !f ) { err = "cannot read image config"; return false; }
		std::string s(( std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		try { blob = JSON::parse(s); } catch ( const std::exception& e ) { err = std::string("image config parse: ") + e.what(); return false; }
	}
	if ( blob.type() != JSON::TYPE::OBJECT ) blob = JSON::Object();
	if ( !blob.contains("config") || blob["config"].type() != JSON::TYPE::OBJECT ) blob["config"] = JSON::Object();
	JSON& cfg = blob["config"];   // reference into blob; stable while we touch only this subtree

	std::string df_wd = "/";
	if ( cfg.contains("WorkingDir") && cfg["WorkingDir"].type() == JSON::TYPE::STRING && !cfg["WorkingDir"].to_string().empty())
		df_wd = cfg["WorkingDir"].to_string();
	std::string df_env;   // accumulated "export K=\"V\"; " prefix applied to each RUN

	std::vector<std::string> lines = read_logical_lines(dockerfile_path, err);
	if ( !err.empty()) return false;

	Mounts mnt(rootfs_dir);   // RAII: unmounts on every return path below
	if ( !mnt.ok ) { err = mnt.err; return false; }

	for ( const std::string& dl : lines ) {
		if ( work::cancelled ) { err = "cancelled"; return false; }
		std::vector<std::string> tk = split_ws(dl);
		if ( tk.empty()) continue;
		std::string kw  = upper(tk[0]);
		std::string arg = after_first_word(dl);

		if ( kw == "FROM" ) {
			// base image, already pulled

		} else if ( kw == "RUN" ) {
			logger::info << "  RUN " << arg << std::endl;
			std::string script = df_env + "cd \"" + df_wd + "\" 2>/dev/null || true\n" + arg;
			std::string rerr;
			if ( !run_in_chroot(rootfs_dir, script, rerr)) {
				err = "RUN failed: " + arg + ( rerr.empty() ? "" : " (" + rerr + ")" );
				return false;
			}

		} else if ( kw == "COPY" || kw == "ADD" ) {
			std::vector<std::string> t = split_ws(arg);
			std::vector<std::string>::size_type start = 0;
			while ( t.size() - start > 1 && t[start].rfind("--", 0) == 0 ) ++start;   // skip --flags
			if ( t.size() - start < 2 ) { err = kw + " needs <src>... <dst>: " + dl; return false; }
			const std::string& dst = t.back();
			std::string ddst = rootfs_dir + "/" + dst;
			if ( !dst.empty() && dst.back() == '/' ) mkdir_p(ddst);
			else mkdir_p(dirname_of(ddst));
			for ( std::vector<std::string>::size_type j = start; j + 1 < t.size(); ++j ) {
				std::string cerr;
				if ( !run_cp(context_dir + "/" + t[j], ddst, cerr)) { err = kw + ": cannot copy " + t[j] + " (" + cerr + ")"; return false; }
			}
			logger::info << "  " << kw << " -> " << dst << std::endl;

		} else if ( kw == "ENV" ) {
			std::string k, v;
			std::string::size_type eq = arg.find('=');
			if ( eq != std::string::npos ) { k = arg.substr(0, eq); v = arg.substr(eq + 1); }
			else {
				std::string::size_type sp = arg.find(' ');
				if ( sp != std::string::npos ) { k = arg.substr(0, sp); v = arg.substr(sp + 1); }
				else { k = arg; v = arg; }
			}
			if ( v.size() >= 2 && (( v.front() == '"' && v.back() == '"' ) || ( v.front() == '\'' && v.back() == '\'' )))
				v = v.substr(1, v.size() - 2);
			JSON ne = JSON::Array();
			std::string pfx = k + "=";
			if ( cfg.contains("Env") && cfg["Env"].type() == JSON::TYPE::ARRAY ) {
				JSON e = cfg["Env"];
				for ( auto it = e.begin(); it != e.end(); ++it ) {
					std::string ev = ( *it.value()).to_string();
					if ( ev.compare(0, pfx.size(), pfx) == 0 ) continue;   // drop the existing K=
					ne.append(JSON(ev));
				}
			}
			ne.append(JSON(k + "=" + v));
			cfg["Env"] = ne;
			df_env += "export " + k + "=\"" + v + "\"; ";
			logger::info << "  ENV " << k << "=" << v << std::endl;

		} else if ( kw == "WORKDIR" ) {
			df_wd = arg;
			mkdir_p(rootfs_dir + "/" + arg);
			cfg["WorkingDir"] = arg;
			logger::verbose << "  WORKDIR " << arg << std::endl;

		} else if ( kw == "USER" ) {
			cfg["User"] = arg;
			logger::verbose << "  USER " << arg << std::endl;

		} else if ( kw == "CMD" ) {
			if ( !set_cmd_like(cfg, "Cmd", "CMD", arg, err)) return false;

		} else if ( kw == "ENTRYPOINT" ) {
			if ( !set_cmd_like(cfg, "Entrypoint", "ENTRYPOINT", arg, err)) return false;

		} else if ( kw == "EXPOSE" || kw == "VOLUME" || kw == "LABEL" || kw == "ARG" ||
		            kw == "MAINTAINER" || kw == "SHELL" || kw == "STOPSIGNAL" ||
		            kw == "HEALTHCHECK" || kw == "ONBUILD" ) {
			logger::verbose << "  (" << kw << " ignored)" << std::endl;

		} else {
			logger::info << "  WARNING: unknown Dockerfile instruction ignored: " << kw << std::endl;
		}
	}

	// write back the mutated config (cfg is a view into blob, so dump blob)
	std::string tmp = config_path + ".tmp";
	{
		std::ofstream of(tmp);
		if ( !of ) { err = "cannot write " + tmp; return false; }
		of << blob.dump(true) << "\n";
		if ( !of ) { err = "write error on " + tmp; return false; }
	}
	if ( rename(tmp.c_str(), config_path.c_str()) != 0 ) { err = "cannot replace " + config_path; return false; }
	return true;   // Mounts dtor unmounts here
}

} // namespace dockerfile
