#include "bundle.hpp"
#include "json.hpp"

#include <fstream>
#include <iterator>
#include <vector>
#include <cstdlib>

namespace bundle {

static const char* CAPS_PERMISSIVE[] = {
	"CAP_CHOWN","CAP_DAC_OVERRIDE","CAP_DAC_READ_SEARCH","CAP_FOWNER","CAP_FSETID","CAP_KILL",
	"CAP_SETGID","CAP_SETUID","CAP_SETPCAP","CAP_NET_BIND_SERVICE","CAP_NET_RAW","CAP_NET_ADMIN",
	"CAP_SYS_CHROOT","CAP_MKNOD","CAP_AUDIT_WRITE","CAP_SETFCAP","CAP_IPC_LOCK","CAP_SYS_PTRACE",
	"CAP_SYS_NICE","CAP_SYS_RESOURCE", nullptr
};
static const char* CAPS_MINIMAL[] = {
	"CAP_CHOWN","CAP_DAC_OVERRIDE","CAP_FOWNER","CAP_SETGID","CAP_SETUID","CAP_NET_BIND_SERVICE","CAP_KILL", nullptr
};

static JSON caps_array(const std::string& set) {
	JSON a = JSON::Array();
	for ( const char** p = ( set == "minimal" ) ? CAPS_MINIMAL : CAPS_PERMISSIVE; *p; ++p )
		a.append(JSON(std::string(*p)));
	return a;
}

// look up `name` in rootfs/etc/<file>, return numeric field `idx` (2=uid/gid id,
// 3=gid in passwd), or -1 if not found.
static long etc_lookup(const std::string& rootfs, const char* file, const std::string& name, int idx) {
	std::ifstream f(rootfs + "/etc/" + file);
	std::string line;
	while ( std::getline(f, line)) {
		if ( line.size() <= name.size() || line.compare(0, name.size(), name) != 0 || line[name.size()] != ':' )
			continue;
		std::vector<std::string> parts; std::string cur;
		for ( char c : line ) { if ( c == ':' ) { parts.push_back(cur); cur.clear(); } else cur += c; }
		parts.push_back(cur);
		if ( (int)parts.size() > idx ) return strtol(parts[idx].c_str(), nullptr, 10);
		return -1;
	}
	return -1;
}

static bool numeric(const std::string& s) {
	return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

static void resolve_user(const std::string& user, const std::string& rootfs, long& uid, long& gid) {
	uid = 0; gid = 0;
	if ( user.empty()) return;
	std::string u = user, g;
	std::string::size_type colon = user.find(':');
	if ( colon != std::string::npos ) { u = user.substr(0, colon); g = user.substr(colon + 1); }
	if ( numeric(u)) uid = strtol(u.c_str(), nullptr, 10);
	else if ( !u.empty()) {
		long x = etc_lookup(rootfs, "passwd", u, 2); if ( x >= 0 ) uid = x;
		if ( g.empty()) { long gg = etc_lookup(rootfs, "passwd", u, 3); if ( gg >= 0 ) gid = gg; }
	}
	if ( !g.empty()) {
		if ( numeric(g)) gid = strtol(g.c_str(), nullptr, 10);
		else { long x = etc_lookup(rootfs, "group", g, 2); if ( x >= 0 ) gid = x; }
	}
}

bool write_config(const std::string& image_config_blob, const std::string& rootfs_dir,
                  const Opts& opts, const std::string& out_config_json, std::string& err) {
	std::ifstream f(image_config_blob);
	if ( !f ) { err = "cannot read image config"; return false; }
	std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	JSON blob;
	try { blob = JSON::parse(s); } catch ( const std::exception& e ) { err = std::string("image config parse: ") + e.what(); return false; }
	JSON c = ( blob.type() == JSON::TYPE::OBJECT && blob.contains("config")) ? blob["config"] : JSON::Object();

	// args = Entrypoint + Cmd, default ["/bin/sh"]
	JSON args = JSON::Array();
	for ( const char* key : { "Entrypoint", "Cmd" } ) {
		if ( c.contains(key) && c[key].type() == JSON::TYPE::ARRAY ) {
			JSON a = c[key];
			for ( auto it = a.begin(); it != a.end(); ++it ) args.append(JSON(( *it.value()).to_string()));
		}
	}
	bool any = false; for ( auto it = args.begin(); it != args.end(); ++it ) { any = true; break; }
	if ( !any ) args.append(JSON(std::string("/bin/sh")));

	// env + default PATH if absent
	JSON env = JSON::Array(); bool has_path = false;
	if ( c.contains("Env") && c["Env"].type() == JSON::TYPE::ARRAY ) {
		JSON e = c["Env"];
		for ( auto it = e.begin(); it != e.end(); ++it ) {
			std::string v = ( *it.value()).to_string();
			if ( v.compare(0, 5, "PATH=") == 0 ) has_path = true;
			env.append(JSON(v));
		}
	}
	if ( !has_path ) env.append(JSON(std::string("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")));

	std::string cwd = "/";
	if ( c.contains("WorkingDir") && c["WorkingDir"].type() == JSON::TYPE::STRING && !c["WorkingDir"].to_string().empty())
		cwd = c["WorkingDir"].to_string();

	long uid, gid;
	resolve_user(( c.contains("User") && c["User"].type() == JSON::TYPE::STRING ) ? c["User"].to_string() : "", rootfs_dir, uid, gid);

	JSON cfg = JSON::Object();
	cfg["ociVersion"] = std::string("1.0.2");
	cfg["hostname"] = opts.name;

	JSON proc = JSON::Object();
	proc["terminal"] = false;
	{ JSON u = JSON::Object(); u["uid"] = (long long)uid; u["gid"] = (long long)gid; proc["user"] = u; }
	proc["cwd"] = cwd;
	proc["args"] = args;
	proc["env"] = env;
	{ JSON caps = JSON::Object(); JSON cs = caps_array(opts.caps);
	  caps["bounding"] = cs; caps["effective"] = cs; caps["inheritable"] = cs; caps["permitted"] = cs; caps["ambient"] = JSON::Array();
	  proc["capabilities"] = caps; }
	{ JSON rl = JSON::Array(); JSON r = JSON::Object();
	  r["type"] = std::string("RLIMIT_NOFILE"); r["hard"] = (long long)1048576; r["soft"] = (long long)1048576;
	  rl.append(r); proc["rlimits"] = rl; }
	proc["noNewPrivileges"] = opts.nnp;
	cfg["process"] = proc;

	{ JSON root = JSON::Object(); root["path"] = std::string("rootfs"); root["readonly"] = opts.rw_overlay; cfg["root"] = root; }

	JSON mounts = JSON::Array();
	auto add_mount = [&](const char* dest, const char* type, const char* src, std::vector<const char*> o) {
		JSON m = JSON::Object();
		m["destination"] = std::string(dest); m["type"] = std::string(type); m["source"] = std::string(src);
		if ( !o.empty()) { JSON oa = JSON::Array(); for ( const char* x : o ) oa.append(JSON(std::string(x))); m["options"] = oa; }
		mounts.append(m);
	};
	add_mount("/proc", "proc", "proc", {});
	add_mount("/sys", "sysfs", "sysfs", { "nosuid", "noexec", "nodev", "ro" });
	add_mount("/tmp", "tmpfs", "tmpfs", { "nosuid", "nodev", "mode=1777" });
	add_mount("/run", "tmpfs", "tmpfs", { "nosuid", "nodev", "mode=0755" });
	if ( opts.resolvconf ) add_mount("/etc/resolv.conf", "bind", "/etc/resolv.conf", { "rbind", "ro" });
	cfg["mounts"] = mounts;

	JSON lx = JSON::Object();
	{ JSON ns = JSON::Array();
	  auto add_ns = [&](const char* t) { JSON n = JSON::Object(); n["type"] = std::string(t); ns.append(n); };
	  add_ns("pid"); if ( opts.network_isolated ) add_ns("network"); add_ns("ipc"); add_ns("uts"); add_ns("cgroup"); add_ns("mount");
	  lx["namespaces"] = ns; }
	if ( opts.accounting ) {
		long pidmax = 32768; { std::ifstream pf("/proc/sys/kernel/pid_max"); if ( pf ) pf >> pidmax; }
		JSON res = JSON::Object();
		JSON mem = JSON::Object(); mem["limit"] = (long long)-1; res["memory"] = mem;
		JSON pids = JSON::Object(); pids["limit"] = (long long)pidmax; res["pids"] = pids;
		lx["resources"] = res;
	}
	cfg["linux"] = lx;

	std::ofstream of(out_config_json);
	if ( !of ) { err = "cannot write " + out_config_json; return false; }
	of << cfg.dump(true) << "\n";
	if ( !of ) { err = "write error on " + out_config_json; return false; }
	return true;
}

}
