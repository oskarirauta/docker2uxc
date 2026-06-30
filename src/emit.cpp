#include "emit.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <set>

namespace emit {
namespace {

std::string read_file(const std::string& path, bool& ok) {
	std::ifstream f(path, std::ios::binary);
	if ( !f ) { ok = false; return ""; }
	ok = true;
	return std::string(( std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool write_file(const std::string& path, const std::string& data, std::string& err) {
	std::ofstream o(path, std::ios::binary);
	if ( !o ) { err = "cannot write " + path; return false; }
	o << data;
	if ( !o ) { err = "write error on " + path; return false; }
	return true;
}

// jq @sh: single-quote, embedded ' -> '\''
std::string sh_quote(const std::string& s) {
	std::string r = "'";
	for ( char c : s ) { if ( c == '\'' ) r += "'\\''"; else r += c; }
	r += "'";
	return r;
}

// recursive deep-merge matching the shell's jq deepmerge($a;$b)
JSON deepmerge(const JSON& a, const JSON& b) {
	if ( a.type() == JSON::TYPE::OBJECT && b.type() == JSON::TYPE::OBJECT ) {
		JSON r = a;
		for ( auto it = b.begin(); it != b.end(); ++it ) {
			std::string k = it.key();
			JSON bv = it.value();
			if ( !r.contains(k) || r[k].type() == JSON::TYPE::NULLPTR ) r[k] = bv;
			else r[k] = deepmerge(r[k], bv);
		}
		return r;
	}
	if ( a.type() == JSON::TYPE::ARRAY && b.type() == JSON::TYPE::ARRAY ) {
		JSON r = a;
		for ( auto it = b.begin(); it != b.end(); ++it ) r.append(it.value());
		return r;
	}
	return b;
}

// recursive: drop object keys starting with '_' (profile comments)
JSON strip_underscore(const JSON& v) {
	if ( v.type() == JSON::TYPE::OBJECT ) {
		JSON r = JSON::Object();
		for ( auto it = v.begin(); it != v.end(); ++it ) {
			std::string k = it.key();
			if ( !k.empty() && k[0] == '_' ) continue;
			r[k] = strip_underscore(it.value());
		}
		return r;
	}
	if ( v.type() == JSON::TYPE::ARRAY ) {
		JSON r = JSON::Array();
		for ( auto it = v.begin(); it != v.end(); ++it ) r.append(strip_underscore(it.value()));
		return r;
	}
	return v;
}

// list the keys of an object field (e.g. .config.ExposedPorts), one "    KEY\n" each
std::string object_keys_block(const JSON& parent, const char* key) {
	std::string out;
	if ( parent.contains(key) && parent[key].type() == JSON::TYPE::OBJECT ) {
		JSON o = parent[key];
		for ( auto it = o.begin(); it != o.end(); ++it ) out += "    " + it.key() + "\n";
	}
	return out;
}

} // anonymous namespace

// Pre-fill web_ports from the image's EXPOSE list: read <out>/image-config.json,
// take .config.ExposedPorts, keep only tcp ports that look like a web UI (a
// curated allow-list - DB/cache/broker ports are deliberately excluded so we
// don't offer a useless "open" link). 443/8443/9443 default to https. Returns a
// JSON array [{ port, scheme? }] (empty if none / unreadable); the user labels
// and trims them in the LuCI editor. register_container only applies this when
// the entry has no web_ports yet, so re-pulls never clobber manual edits.
JSON web_ports_from_image(const std::string& image_config_path) {
	static const std::set<int> web_typical = {
		80, 443, 3000, 3001, 4000, 5000, 7860, 8000, 8008,
		8080, 8081, 8096, 8123, 8443, 8888, 9000, 9090, 9443
	};
	JSON out = JSON::Array();
	bool ok;
	std::string s = read_file(image_config_path, ok);
	if ( !ok ) return out;
	JSON blob; try { blob = JSON::parse(s); } catch ( ... ) { return out; }
	if ( blob.type() != JSON::TYPE::OBJECT || !blob.contains("config")) return out;
	JSON icfg = blob["config"];
	if ( icfg.type() != JSON::TYPE::OBJECT || !icfg.contains("ExposedPorts") ||
	     icfg["ExposedPorts"].type() != JSON::TYPE::OBJECT )
		return out;
	JSON ep = icfg["ExposedPorts"];
	for ( auto it = ep.begin(); it != ep.end(); ++it ) {
		std::string k = it.key();                        // "80/tcp"
		std::string::size_type slash = k.find('/');
		std::string proto = ( slash == std::string::npos ) ? "tcp" : k.substr(slash + 1);
		if ( proto != "tcp" ) continue;                  // web UIs are tcp
		int port = atoi(( slash == std::string::npos ? k : k.substr(0, slash)).c_str());
		if ( port <= 0 || !web_typical.count(port)) continue;
		JSON e = JSON::Object();
		e["port"] = (long long)port;
		if ( port == 443 || port == 8443 || port == 9443 ) e["scheme"] = "https";
		out.append(e);
	}
	return out;
}

std::string profile_dir() {
	const char* env = getenv("DOCKER2UXC_PROFILES");
	if ( env && *env ) return env;
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if ( n > 0 ) {
		buf[n] = '\0';
		std::string exe(buf);
		std::string::size_type s = exe.find_last_of('/');
		std::string dir = ( s == std::string::npos ) ? "." : exe.substr(0, s);
		std::string p = dir + "/profiles";
		struct stat st;
		if ( stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return p;
	}
	return "/usr/share/docker2uxc/profiles";
}

bool profile(const std::string& config_path, const std::string& dir, const std::string& name, std::string& err) {
	std::string pf = dir + "/" + name + ".json";
	struct stat st;
	if ( stat(pf.c_str(), &st) != 0 ) { err = "profile not found: " + pf; return false; }

	bool ok;
	std::string cs = read_file(config_path, ok);
	if ( !ok ) { err = "cannot read " + config_path; return false; }
	std::string ps = read_file(pf, ok);
	if ( !ok ) { err = "cannot read " + pf; return false; }

	JSON base, ovl;
	try { base = JSON::parse(cs); ovl = JSON::parse(ps); }
	catch ( const std::exception& e ) { err = std::string("profile merge: ") + e.what(); return false; }

	JSON merged = strip_underscore(deepmerge(base, ovl));
	return write_file(config_path, merged.dump(true) + "\n", err);
}

bool netconfig(const std::string& out, const std::string& name, const std::string& bridge, std::string& err) {
	std::string host = ( "vh-" + name ).substr(0, 15);   // Linux ifname limit
	std::string peer = ( name + "0" ).substr(0, 15);
	std::string s =
		"# docker2uxc: /etc/config/network snippet for isolated container '" + name + "'.\n"
		"# NOT applied automatically - review, edit the bridge/addressing, then apply with:\n"
		"#     cat network.uci >> /etc/config/network && /etc/init.d/network reload\n"
		"# See the uxcd networking docs (docs/networking.md) for the full explanation.\n"
		"\n"
		"# 1) veth pair: host side '" + host + "', container side '" + peer + "'\n"
		"config device\n"
		"\toption type 'veth'\n"
		"\toption name '" + host + "'\n"
		"\toption peer_name '" + peer + "'\n"
		"\n"
		"# 2) attach the host side to a bridge (default '" + bridge + "' - change if needed)\n"
		"config device\n"
		"\toption name '" + bridge + "'\n"
		"\tlist ports '" + host + "'\n"
		"\n"
		"# 3) container-side interface, managed inside the netns by uxc/netifd.\n"
		"#    proto 'infra' lets uxc move '" + peer + "' into the container and wire DNS.\n"
		"config interface '" + name + "'\n"
		"\toption proto 'infra'\n"
		"\toption device '" + peer + "'\n";
	if ( !write_file(out + "/network.uci", s, err)) return false;
	logger::info << "    network: wrote " << out << "/network.uci (isolated mode - review before applying)" << std::endl;
	return true;
}

bool keeper(const std::string& out, const std::string& name, const std::string& abs_bundle, std::string& err) {
	std::string s =
		"#!/bin/sh /etc/rc.common\n"
		"# uxc keeper for the docker2uxc bundle '" + name + "' - gives uxc a Docker-style\n"
		"# auto-restart policy without bypassing uxc (stays in 'uxc list').\n"
		"# Install: cp " + name + ".init /etc/init.d/" + name + "-keeper && chmod +x /etc/init.d/" + name + "-keeper\n"
		"#          /etc/init.d/" + name + "-keeper enable && /etc/init.d/" + name + "-keeper start\n"
		"# Stop with: /etc/init.d/" + name + "-keeper stop  (plain 'uxc kill' would just be\n"
		"# restarted by the keeper, like Docker's restart: always).\n"
		"USE_PROCD=1\n"
		"START=95\n"
		"STOP=10\n"
		"NAME=" + name + "\n"
		"BUNDLE=" + abs_bundle + "\n";
	// body: verbatim from the shell's quoted heredoc (no substitution; $NAME/$BUNDLE
	// resolve at init-script runtime from the NAME=/BUNDLE= lines above)
	s += R"KEEPER(
start_service() {
	procd_open_instance "${NAME}-keeper"
	procd_set_param command /bin/sh -c "
		uxc create $NAME --bundle $BUNDLE 2>/dev/null
		while true; do
			st=\$(uxc state $NAME 2>/dev/null | jsonfilter -e '@.status' 2>/dev/null)
			if [ \"\$st\" != running ]; then
				# Recover: re-create (no --bundle - the path is stored, and
				# passing it again errors 'File exists'), then poll 'uxc start'
				# until it takes. 'uxc create' returns before the container is
				# start-ready, and how long that takes scales with image size,
				# so poll instead of a fixed sleep.
				uxc create $NAME 2>/dev/null
				n=0
				until uxc start $NAME 2>/dev/null || [ \$n -ge 60 ]; do n=\$((n+1)); sleep 1; done
			fi
			sleep 5
		done
	"
	procd_set_param respawn
	procd_close_instance
}

stop_service() {
	uxc kill "$NAME" 2>/dev/null
}
)KEEPER";
	if ( !write_file(out + "/" + name + ".init", s, err)) return false;
	logger::info << "    init: wrote " << out << "/" << name << ".init (uxc keeper - auto-restart, stays uxc-managed)" << std::endl;
	return true;
}

bool notes(const NotesInfo& ni, std::string& err) {
	bool ok;
	std::string cfg_s = read_file(ni.config_json_path, ok);   // for process.args
	JSON cfg; if ( ok ) { try { cfg = JSON::parse(cfg_s); } catch ( ... ) {} }
	std::string blob_s = read_file(ni.image_config_path, ok); // for ExposedPorts/Volumes
	JSON blob; if ( ok ) { try { blob = JSON::parse(blob_s); } catch ( ... ) {} }
	JSON icfg = ( blob.type() == JSON::TYPE::OBJECT && blob.contains("config")) ? blob["config"] : JSON::Object();

	// .process.args | "    " + (map(@sh) | join(" "))
	std::string args_line = "    ";
	if ( cfg.type() == JSON::TYPE::OBJECT && cfg.contains("process") && cfg["process"].contains("args")) {
		JSON a = cfg["process"]["args"];
		bool first = true;
		for ( auto it = a.begin(); it != a.end(); ++it ) { if ( !first ) args_line += " "; args_line += sh_quote(( *it.value()).to_string()); first = false; }
	}

	std::string s;
	s += "# " + ni.name + " - generated by docker2uxc " + ni.version + "\n\n";
	s += "Source image : " + ni.source + " (" + ni.arch + ")\n";
	s += "Config digest: " + ni.config_digest + "\n\n";
	s += "## Entrypoint / Cmd\n";
	s += args_line + "\n\n";
	s += "## Network: " + ni.network + "\n";
	if ( ni.network == "host" ) {
		s += "    Shares the host network namespace - no /etc/config/network setup needed.\n";
	} else {
		s += "    Own network namespace. Apply the generated network.uci to\n";
		s += "    /etc/config/network (review it first) or DNS/connectivity will fail:\n";
		s += "        cat network.uci >> /etc/config/network && /etc/init.d/network reload\n";
	}
	s += "\n";
	s += "## Exposed ports (from image - uxc does no port mapping)\n";
	s += object_keys_block(icfg, "ExposedPorts");
	s += "\n";
	s += "## Volumes declared by image (MOUNT THESE BY HAND in config.json)\n";
	s += object_keys_block(icfg, "Volumes");
	s += "\n";
	s += "Add bind mounts to config.json \"mounts\" like:\n";
	s += "    { \"destination\": \"/data\", \"type\": \"bind\",\n";
	s += "      \"source\": \"/srv/" + ni.name + "/data\", \"options\": [\"rbind\",\"rw\"] }\n";
	s += "\n";
	s += "## Install (option A: uxc)\n";
	s += "    uxc create " + ni.name + " --bundle " + ni.abs_out + ( ni.rw_overlay ? " --write-overlay-path <overlay-dir>" : "" ) + "\n";
	s += "    uxc start  " + ni.name + "\n";
	s += "    uxc enable " + ni.name + "      # autostart on boot\n";
	s += "    NOTE: 'uxc start' may print a cosmetic 'No such file or directory';\n";
	s += "    the container boots asynchronously - check 'uxc state " + ni.name + "'.\n";
	s += "\n";
	if ( ni.emit_keeper ) {
		s += "## Install (option B: uxc keeper - auto-restart, stays uxc-managed)\n";
		s += "    Use this if the app restarts itself (e.g. Frigate UI 'Save & Restart');\n";
		s += "    uxc has no respawn policy, so the plain uxc method won't come back.\n";
		s += "    The keeper still leaves the container in 'uxc list' / uxc state|kill|attach.\n";
		s += "    cp " + ni.name + ".init /etc/init.d/" + ni.name + "-keeper && chmod +x /etc/init.d/" + ni.name + "-keeper\n";
		s += "    /etc/init.d/" + ni.name + "-keeper enable && /etc/init.d/" + ni.name + "-keeper start\n";
	}
	return write_file(ni.out + "/README.notes", s, err);
}

void warn_missing_binds(const std::string& config_json_path) {
	bool ok;
	std::string s = read_file(config_json_path, ok);
	if ( !ok ) return;
	JSON cfg;
	try { cfg = JSON::parse(s); } catch ( ... ) { return; }
	if ( cfg.type() != JSON::TYPE::OBJECT || !cfg.contains("mounts") || cfg["mounts"].type() != JSON::TYPE::ARRAY ) return;

	std::vector<std::string> missing;
	JSON m = cfg["mounts"];
	for ( auto it = m.begin(); it != m.end(); ++it ) {
		JSON e = *it.value();
		if ( !e.contains("type") || e["type"].to_string() != "bind" ) continue;
		if ( !e.contains("source")) continue;
		std::string src = e["source"].to_string();
		struct stat st;
		if ( stat(src.c_str(), &st) != 0 ) missing.push_back(src);
	}
	if ( missing.empty()) return;

	logger::info << "" << std::endl;
	logger::info << "WARNING: these bind mount sources do not exist on the host yet:" << std::endl;
	for ( const std::string& src : missing ) logger::info << "    " << src << std::endl;
	logger::info << "  Create them (mkdir -p ...) before 'uxc start', or ujail will refuse to" << std::endl;
	logger::info << "  build the container ('jail: mount_all() failed')." << std::endl;
}

}
