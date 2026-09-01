#include "emit.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
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

std::string mount_dest(const JSON& m) {
	return ( m.type() == JSON::TYPE::OBJECT && m.contains("destination")) ? m["destination"].to_string() : "";
}

// Merge two mount arrays BY DESTINATION: an overlay mount replaces the base
// mount at the same path (keeping the base's position), anything new is
// appended. Plain concatenation leaves two mounts on one destination, and ujail
// then rejects the ENTIRE spec ("parsing of OCI JSON spec has failed") - the
// classic failure of a profile that re-binds /config or /dev/shm.
JSON merge_mounts(const JSON& base, const JSON& ovl) {
	JSON r = JSON::Array();
	for ( auto bi = base.begin(); bi != base.end(); ++bi ) {
		JSON e = bi.value();
		std::string d = mount_dest(e);
		for ( auto oi = ovl.begin(); oi != ovl.end(); ++oi )
			if ( !d.empty() && mount_dest(oi.value()) == d ) { e = oi.value(); break; }
		r.append(e);
	}
	for ( auto oi = ovl.begin(); oi != ovl.end(); ++oi ) {
		JSON e = oi.value();
		std::string d = mount_dest(e);
		bool seen = false;
		for ( auto bi = base.begin(); bi != base.end(); ++bi )
			if ( !d.empty() && mount_dest(bi.value()) == d ) { seen = true; break; }
		if ( !seen ) r.append(e);
	}
	return r;
}

// recursive deep-merge matching the shell's jq deepmerge($a;$b)
JSON deepmerge(const JSON& a, const JSON& b) {
	if ( a.type() == JSON::TYPE::OBJECT && b.type() == JSON::TYPE::OBJECT ) {
		JSON r = a;
		for ( auto it = b.begin(); it != b.end(); ++it ) {
			std::string k = it.key();
			JSON bv = it.value();
			// OCI capability sets REPLACE, not concatenate, so a profile can scope
			// caps DOWN. To ADD to the --caps set - what an application profile
			// almost always wants - use "_caps_add" instead (apply_caps_add()).
			bool cap_set = ( k == "bounding" || k == "effective" || k == "permitted" ||
			                 k == "inheritable" || k == "ambient" );
			if ( k == "mounts" && r.contains(k) &&
			     r[k].type() == JSON::TYPE::ARRAY && bv.type() == JSON::TYPE::ARRAY )
				r[k] = merge_mounts(r[k], bv);
			else if ( !r.contains(k) || r[k].type() == JSON::TYPE::NULLPTR || cap_set ) r[k] = bv;
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

// "_caps_add": [...] - union the listed capabilities into every set the base
// config grants (ambient is left alone: it is empty by design). A profile author
// means "frigate ALSO needs CAP_SYS_ADMIN and CAP_PERFMON", not "frigate runs
// with ONLY those two" - the latter silently drops CAP_CHOWN and the container
// dies on the first chown its init does.
void apply_caps_add(JSON& cfg, const std::vector<std::string>& add) {
	if ( add.empty()) return;
	if ( !cfg.contains("process") || cfg["process"].type() != JSON::TYPE::OBJECT ) return;
	if ( !cfg["process"].contains("capabilities")) return;
	for ( const char* set : { "bounding", "effective", "permitted", "inheritable" } ) {
		JSON cur = cfg["process"]["capabilities"].contains(set) ? cfg["process"]["capabilities"][set] : JSON::Array();
		if ( cur.type() != JSON::TYPE::ARRAY ) cur = JSON::Array();
		std::set<std::string> have;
		for ( auto it = cur.begin(); it != cur.end(); ++it ) have.insert(it.value() -> to_string());
		for ( const std::string& c : add )
			if ( have.insert(c).second ) cur.append(JSON(c));
		cfg["process"]["capabilities"][set] = cur;
	}
}

// Drop mounts the profile marked "_optional" whose host source is absent, so a
// single profile can offer /dev/dri AND /dev/bus/usb and still start on a box
// that has neither (ujail fails the whole container on one missing bind source).
void drop_absent_optional(JSON& cfg) {
	if ( !cfg.contains("mounts") || cfg["mounts"].type() != JSON::TYPE::ARRAY ) return;
	JSON kept = JSON::Array();
	for ( auto it = cfg["mounts"].begin(); it != cfg["mounts"].end(); ++it ) {
		JSON m = *it.value();
		bool opt = ( m.type() == JSON::TYPE::OBJECT && m.contains("_optional") && m["_optional"].to_bool());
		std::string src = ( m.type() == JSON::TYPE::OBJECT && m.contains("source")) ? m["source"].to_string() : "";
		struct stat ost;
		if ( opt && !src.empty() && src[0] == '/' && stat(src.c_str(), &ost) != 0 ) {
			logger::info << "    optional mount skipped: " << mount_dest(m)
			             << " (no " << src << " on this host)" << std::endl;
			continue;
		}
		kept.append(m);
	}
	cfg["mounts"] = kept;
}

// Pull the non-OCI half out of a parsed profile: description, the _registry
// block, _caps_add, and the host paths its binds/volumes will need.
void collect_info(const JSON& p, const std::string& name, ProfileInfo& info) {
	info.name = name;
	if ( p.contains("_description")) info.description = p["_description"].to_string();
	if ( p.contains("_registry") && p["_registry"].type() == JSON::TYPE::OBJECT )
		info.registry = strip_underscore(p["_registry"]);
	if ( p.contains("_seed") && p["_seed"].type() == JSON::TYPE::OBJECT )
		info.seed = p["_seed"];
	if ( p.contains("_matches") && p["_matches"].type() == JSON::TYPE::ARRAY ) {
		const JSON ma = p["_matches"];
		for ( auto it = ma.begin(); it != ma.end(); ++it ) info.matches.push_back(it.value().to_string());
	}
	// NOTE: JSON's const operator[] returns BY VALUE, so every loop below binds
	// the array to a local first - begin() and end() taken from two separate
	// temporaries do not compare.
	if ( p.contains("_caps_add") && p["_caps_add"].type() == JSON::TYPE::ARRAY ) {
		const JSON ca = p["_caps_add"];
		for ( auto it = ca.begin(); it != ca.end(); ++it )
			info.caps_add.push_back(it.value().to_string());
	}
	auto need = [&](const std::string& path) {
		if ( path.empty() || path[0] != '/' ) return;
		for ( const std::string& e : info.needs ) if ( e == path ) return;
		info.needs.push_back(path);
	};
	if ( p.contains("mounts") && p["mounts"].type() == JSON::TYPE::ARRAY ) {
		const JSON ms = p["mounts"];
		for ( auto it = ms.begin(); it != ms.end(); ++it ) {
			JSON m = it.value();
			if ( m.type() != JSON::TYPE::OBJECT ) continue;
			if ( m.contains("type") && m["type"].to_string() != "bind" ) continue;
			if ( m.contains("_optional") && m["_optional"].to_bool()) continue;
			if ( m.contains("source")) need(m["source"].to_string());
		}
	}
	if ( info.registry.contains("volumes") && info.registry["volumes"].type() == JSON::TYPE::ARRAY ) {
		const JSON vs = info.registry["volumes"];
		for ( auto it = vs.begin(); it != vs.end(); ++it ) {
			std::string v = it.value().to_string();
			std::string::size_type c = v.find(':');
			need( c == std::string::npos ? v : v.substr(0, c));
		}
	}
	if ( info.registry.contains("devices") && info.registry["devices"].type() == JSON::TYPE::ARRAY ) {
		const JSON ds = info.registry["devices"];
		for ( auto it = ds.begin(); it != ds.end(); ++it )
			info.devices.push_back(it.value().to_string());
	}
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

// Pre-fill stop_signal from the image's STOPSIGNAL (.config.StopSignal). Postgres
// ships SIGINT, nginx SIGQUIT - carry it so the daemon stops them cleanly.
std::string stop_signal_from_image(const std::string& image_config_path) {
	bool ok;
	std::string s = read_file(image_config_path, ok);
	if ( !ok ) return "";
	JSON blob; try { blob = JSON::parse(s); } catch ( ... ) { return ""; }
	if ( blob.type() != JSON::TYPE::OBJECT || !blob.contains("config")) return "";
	JSON icfg = blob["config"];
	if ( icfg.type() != JSON::TYPE::OBJECT || !icfg.contains("StopSignal")) return "";
	return icfg["StopSignal"].to_string();
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

std::vector<std::string> seed_files(const JSON& seed) {
	std::vector<std::string> written;
	if ( seed.type() != JSON::TYPE::OBJECT ) return written;
	for ( auto it = seed.begin(); it != seed.end(); ++it ) {
		std::string path = it.key();
		if ( path.empty() || path[0] != '/' ) continue;      // absolute host paths only
		struct stat st;
		if ( stat(path.c_str(), &st) == 0 ) continue;        // never overwrite the user's file
		std::string parent = path.substr(0, path.find_last_of('/'));
		for ( std::string::size_type i = 1; i <= parent.size(); ++i )   // mkdir -p
			if ( i == parent.size() || parent[i] == '/' ) mkdir(parent.substr(0, i).c_str(), 0755);
		std::string werr;
		if ( write_file(path, it.value().to_string(), werr)) written.push_back(path);
		else logger::error << "seed: " << werr << std::endl;
	}
	return written;
}

std::vector<std::string> profile_names(const std::string& dir) {
	std::vector<std::string> names;
	DIR* d = opendir(dir.c_str());
	if ( !d ) return names;
	for ( struct dirent* e; ( e = readdir(d)) != nullptr; ) {
		std::string fn = e -> d_name;
		if ( fn.size() <= 5 || fn.substr(fn.size() - 5) != ".json" ) continue;
		std::string n = fn.substr(0, fn.size() - 5);
		if ( n.empty() || n[0] == '_' ) continue;     // _template and friends
		names.push_back(n);
	}
	closedir(d);
	std::sort(names.begin(), names.end());
	return names;
}

// A "profile not found" is nearly always a typo - say what IS available.
static std::string not_found(const std::string& dir, const std::string& name) {
	std::string m = "profile not found: " + dir + "/" + name + ".json";
	std::vector<std::string> have = profile_names(dir);
	if ( !have.empty()) {
		m += " (available:";
		for ( const std::string& n : have ) m += " " + n;
		m += ")";
	}
	return m;
}

bool profile_info(const std::string& dir, const std::string& name, ProfileInfo& out, std::string& err) {
	std::string pf = dir + "/" + name + ".json";
	struct stat st;
	if ( stat(pf.c_str(), &st) != 0 ) { err = not_found(dir, name); return false; }
	bool ok;
	std::string ps = read_file(pf, ok);
	if ( !ok ) { err = "cannot read " + pf; return false; }
	JSON p;
	try { p = JSON::parse(ps); }
	catch ( const std::exception& e ) { err = std::string("profile ") + name + ": " + e.what(); return false; }
	collect_info(p, name, out);
	return true;
}

std::string match_profile(const std::string& dir, const std::string& image_ref) {
	if ( image_ref.empty()) return "";
	// strip the tag/digest, then the registry host, leaving "blakeblackshear/frigate"
	std::string repo = image_ref;
	std::string::size_type at = repo.find('@');
	if ( at != std::string::npos ) repo = repo.substr(0, at);
	std::string::size_type slash = repo.find('/');
	std::string::size_type colon = repo.rfind(':');
	if ( colon != std::string::npos && ( slash == std::string::npos || colon > slash )) repo = repo.substr(0, colon);
	// a first component with a dot or a port is a registry host, not a namespace
	std::string::size_type s = repo.find('/');
	if ( s != std::string::npos ) {
		std::string head = repo.substr(0, s);
		if ( head.find('.') != std::string::npos || head.find(':') != std::string::npos ) repo = repo.substr(s + 1);
	}
	std::string leaf = repo.substr(repo.find_last_of('/') == std::string::npos ? 0 : repo.find_last_of('/') + 1);

	for ( const std::string& n : profile_names(dir)) {
		ProfileInfo pi;
		std::string perr;
		if ( !profile_info(dir, n, pi, perr)) continue;
		for ( const std::string& m : pi.matches )
			if ( m == leaf || m == repo ) return n;
	}
	return "";
}

bool profile(const std::string& config_path, const std::string& dir, const std::string& name,
             ProfileInfo* info, std::string& err) {
	std::string pf = dir + "/" + name + ".json";
	struct stat st;
	if ( stat(pf.c_str(), &st) != 0 ) { err = not_found(dir, name); return false; }

	bool ok;
	std::string cs = read_file(config_path, ok);
	if ( !ok ) { err = "cannot read " + config_path; return false; }
	std::string ps = read_file(pf, ok);
	if ( !ok ) { err = "cannot read " + pf; return false; }

	JSON base, ovl;
	try { base = JSON::parse(cs); ovl = JSON::parse(ps); }
	catch ( const std::exception& e ) { err = std::string("profile merge: ") + e.what(); return false; }

	ProfileInfo pi;
	collect_info(ovl, name, pi);

	JSON merged = deepmerge(base, ovl);
	apply_caps_add(merged, pi.caps_add);
	drop_absent_optional(merged);
	merged = strip_underscore(merged);

	if ( !pi.description.empty()) logger::info << "    " << pi.description << std::endl;
	if ( !pi.caps_add.empty()) {
		std::string s;
		for ( const std::string& c : pi.caps_add ) s += ( s.empty() ? "" : " " ) + c;
		logger::info << "    capabilities added: " << s << std::endl;
	}
	// A profile that hands over its own capability sets REPLACES them; say so,
	// because narrowing them is exactly how a container ends up unable to chown.
	if ( ovl.contains("process") && ovl["process"].type() == JSON::TYPE::OBJECT &&
	     ovl["process"].contains("capabilities"))
		logger::info << "    note: this profile REPLACES the capability set (--caps is ignored)" << std::endl;

	if ( info ) *info = pi;
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
