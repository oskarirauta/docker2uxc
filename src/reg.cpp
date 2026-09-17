#include "reg.hpp"
#include "json.hpp"

#include <fstream>
#include <iterator>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>

namespace reg {
namespace {

// Read <uxc_dir>/<name>.json, or an empty object when it does not exist / is not
// an object. A corrupt entry is reported so a re-register does not silently
// discard whatever the user had in it.
JSON read_entry(const std::string& path, bool& parse_failed) {
	parse_failed = false;
	JSON entry = JSON::Object();
	std::ifstream f(path);
	if ( !f ) return entry;
	std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	try {
		JSON e = JSON::parse(s);
		if ( e.type() == JSON::TYPE::OBJECT ) return e;
	} catch ( ... ) { parse_failed = true; }
	return entry;
}

// Write the entry through a temp file + rename, so a crash or a full disk can
// never leave a half-written registry entry behind (uxcd reads these at boot).
// 0600: the entry may carry env secrets added via uxcd.
bool write_entry(const std::string& path, const JSON& entry, std::string& err) {
	std::string tmp = path + ".tmp";
	{
		std::ofstream of(tmp);
		if ( !of ) { err = "cannot write " + tmp; return false; }
		of << entry.dump(true) << "\n";
		if ( !of ) { err = "write error on " + tmp; std::remove(tmp.c_str()); return false; }
	}
	chmod(tmp.c_str(), 0600);
	if ( rename(tmp.c_str(), path.c_str()) != 0 ) { err = "cannot replace " + path; std::remove(tmp.c_str()); return false; }
	return true;
}

}

bool register_container(const std::string& uxc_dir, const std::string& name, const std::string& abs_out,
                        const std::string& image, const std::string& digest, const std::string& infra,
                        bool autostart, const JSON& web_ports, const std::string& stop_signal,
                        const std::string& write_overlay_path, const JSON& build, std::string& err) {
	mkdir(uxc_dir.c_str(), 0755);
	std::string path = uxc_dir + "/" + name + ".json";

	// start from the existing entry (preserve user overrides), then overlay
	bool corrupt = false;
	JSON merged = read_entry(path, corrupt);
	if ( corrupt ) { err = "cannot parse existing " + path + " - fix or remove it first"; return false; }
	merged["name"] = name;
	merged["path"] = abs_out;
	if ( !image.empty()) merged["image"] = image;
	// created: first registration only; upgraded: the digest changed (a re-pull to a
	// NEW image). A same-digest re-pull is not an upgrade, so compare before overwriting.
	if ( !merged.contains("created")) merged["created"] = (long long)time(nullptr);
	if ( !digest.empty() && merged.contains("digest") && merged["digest"].to_string() != digest )
		merged["upgraded"] = (long long)time(nullptr);
	if ( !digest.empty()) merged["digest"] = digest;
	// Build provenance and image provenance are mutually exclusive: whichever this
	// registration produced wins and the other is dropped. Without this a bundle
	// first pulled and later rebuilt from a Dockerfile keeps a stale `image`, and
	// `uxc upgrade` re-pulls the stock image straight over the built rootfs -
	// exactly how a PHP-FPM container loses its compiled extensions.
	if ( build.type() == JSON::TYPE::OBJECT && build.begin() != build.end()) {
		// "upgraded" for a build: the rootfs is different if EITHER the base image
		// moved or the recipe itself changed. Comparing only the base would leave
		// the timestamp empty after the most ordinary rebuild there is - edit the
		// Dockerfile, upgrade - and the LuCI "Upgraded" row would say nothing
		// happened.
		auto old_of = [&merged](const char* key) -> std::string {
			if ( !merged.contains("build") || merged["build"].type() != JSON::TYPE::OBJECT ) return "";
			return merged["build"].contains(key) ? merged["build"][key].to_string() : std::string();
		};
		auto changed = [&](const char* key) -> bool {
			std::string o = old_of(key);
			std::string n = build.contains(key) ? build[key].to_string() : "";
			return !n.empty() && !o.empty() && o != n;
		};
		if ( changed("base_digest") || changed("dockerfile_sha256"))
			merged["upgraded"] = (long long)time(nullptr);
		merged["build"] = build;
		if ( merged.contains("image"))  merged.erase("image");
		if ( merged.contains("digest")) merged.erase("digest");
	} else if ( !image.empty() && merged.contains("build")) {
		merged.erase("build");
	}
	if ( !infra.empty()) merged["infra"] = infra;       // only when given -> existing infra survives
	if ( autostart ) merged["autostart"] = true;        // only when flagged -> existing survives
	if ( web_ports.type() == JSON::TYPE::ARRAY && web_ports.begin() != web_ports.end() && !merged.contains("web_ports"))
		merged["web_ports"] = web_ports;            // EXPOSE-derived prefill; only when the user has none
	if ( !stop_signal.empty() && !merged.contains("stop_signal"))
		merged["stop_signal"] = stop_signal;        // STOPSIGNAL-derived; only when the user has none
	if ( !write_overlay_path.empty() && !merged.contains("write_overlay_path"))
		merged["write_overlay_path"] = write_overlay_path;   // persistent r/w overlay; user's value survives a re-pull

	return write_entry(path, merged, err);
}

bool apply_profile_registry(const std::string& uxc_dir, const std::string& name, const JSON& block,
                            std::vector<std::string>& applied, std::string& err) {
	if ( block.type() != JSON::TYPE::OBJECT ) return true;
	std::string path = uxc_dir + "/" + name + ".json";

	JSON entry = JSON::Object();
	{
		struct stat st;
		if ( stat(path.c_str(), &st) != 0 ) { err = "no registry entry at " + path; return false; }
		bool corrupt = false;
		entry = read_entry(path, corrupt);
		if ( corrupt ) { err = "cannot parse " + path; return false; }
	}

	bool changed = false;
	for ( auto it = block.begin(); it != block.end(); ++it ) {
		std::string k = it.key();
		if ( k.empty() || k[0] == '_' ) continue;
		// never let a shared profile file redefine identity/provenance
		if ( k == "name" || k == "path" || k == "image" || k == "digest" || k == "created" || k == "build" ) continue;
		if ( entry.contains(k)) continue;              // the user's own value always wins
		entry[k] = it.value();
		applied.push_back(k);
		changed = true;
	}
	if ( !changed ) return true;

	return write_entry(path, entry, err);
}

bool record_recipe(const std::string& uxc_dir, const std::string& name, const std::string& recipe, std::string& err) {
	if ( recipe.empty()) return true;
	std::string path = uxc_dir + "/" + name + ".json";
	struct stat st;
	if ( stat(path.c_str(), &st) != 0 ) { err = "no registry entry at " + path; return false; }
	bool corrupt = false;
	JSON entry = read_entry(path, corrupt);
	if ( corrupt ) { err = "cannot parse " + path; return false; }
	if ( entry.contains("recipe") && entry["recipe"].to_string() == recipe ) return true;   // nothing to do
	entry["recipe"] = recipe;
	return write_entry(path, entry, err);
}

}
