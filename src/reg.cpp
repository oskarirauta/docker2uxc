#include "reg.hpp"
#include "json.hpp"

#include <fstream>
#include <iterator>
#include <cstdio>
#include <sys/stat.h>

namespace reg {

bool register_container(const std::string& uxc_dir, const std::string& name, const std::string& abs_out,
                        const std::string& image, const std::string& digest, const std::string& infra,
                        bool autostart, const JSON& web_ports, const std::string& stop_signal, std::string& err) {
	mkdir(uxc_dir.c_str(), 0755);
	std::string path = uxc_dir + "/" + name + ".json";

	// start from the existing entry (preserve user overrides), then overlay
	JSON merged = JSON::Object();
	{
		std::ifstream f(path);
		if ( f ) {
			std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			try { JSON e = JSON::parse(s); if ( e.type() == JSON::TYPE::OBJECT ) merged = e; } catch ( ... ) {}
		}
	}
	merged["name"] = name;
	merged["path"] = abs_out;
	if ( !image.empty()) merged["image"] = image;
	if ( !digest.empty()) merged["digest"] = digest;
	if ( !infra.empty()) merged["infra"] = infra;       // only when given -> existing infra survives
	if ( autostart ) merged["autostart"] = true;        // only when flagged -> existing survives
	if ( web_ports.type() == JSON::TYPE::ARRAY && web_ports.begin() != web_ports.end() && !merged.contains("web_ports"))
		merged["web_ports"] = web_ports;            // EXPOSE-derived prefill; only when the user has none
	if ( !stop_signal.empty() && !merged.contains("stop_signal"))
		merged["stop_signal"] = stop_signal;        // STOPSIGNAL-derived; only when the user has none

	std::string tmp = path + ".tmp";
	{
		std::ofstream of(tmp);
		if ( !of ) { err = "cannot write " + tmp; return false; }
		of << merged.dump(true) << "\n";
		if ( !of ) { err = "write error on " + tmp; return false; }
	}
	chmod(tmp.c_str(), 0600);   // may later hold env secrets (added via uxcd)
	if ( rename(tmp.c_str(), path.c_str()) != 0 ) { err = "cannot replace " + path; std::remove(tmp.c_str()); return false; }
	return true;
}

}
