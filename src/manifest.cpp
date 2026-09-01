#include "manifest.hpp"
#include "registry.hpp"
#include "json.hpp"
#include "sha256.hpp"

namespace manifest {

bool resolve(const ImageRef& ref, const std::string& arch_base, const std::string& arch_var,
             const std::string& auth_file, Image& out, std::string& err) {
	std::string body;
	if ( !registry::fetch_manifest(ref, auth_file, body, err)) return false;
	out.provenance_digest = "sha256:" + sha256_hex(body);   // digest of the ref-resolved manifest

	// A digest-pinned ref (image@sha256:...) must hash to exactly that digest,
	// else a compromised/hostile registry could serve an arbitrary manifest for
	// the pin and everything downstream would still "verify" against it.
	if ( !ref.digest.empty() && out.provenance_digest != ref.digest ) {
		err = "manifest digest mismatch: pinned " + ref.digest + ", got " + out.provenance_digest;
		return false;
	}

	try {
		JSON m = JSON::parse(body);
		std::string mt = m.contains("mediaType") ? m["mediaType"].to_string() : "";
		bool is_index = m.contains("manifests") &&
		                ( mt.find("image.index") != std::string::npos ||
		                  mt.find("manifest.list") != std::string::npos ||
		                  mt.empty());   // some indexes omit mediaType

		if ( is_index ) {
			JSON arr = m["manifests"];
			std::string sub, fallback;
			for ( auto it = arr.begin(); it != arr.end(); ++it ) {
				JSON e = *it.value();
				if ( !e.contains("platform")) continue;
				JSON p = e["platform"];
				std::string os = p.contains("os")           ? p["os"].to_string()           : "";
				std::string a  = p.contains("architecture") ? p["architecture"].to_string() : "";
				std::string v  = p.contains("variant")      ? p["variant"].to_string()      : "";
				if ( os != "linux" || a != arch_base ) continue;
				std::string dg = e.contains("digest") ? e["digest"].to_string() : "";
				// exact variant, or the canonical default when none was requested
				// (arm64's default variant is v8; most other arches carry none)
				bool match = arch_var.empty()
				             ? ( v.empty() || ( arch_base == "arm64" && v == "v8" ))
				             : ( v == arch_var );
				if ( match ) { sub = dg; break; }
				if ( fallback.empty()) fallback = dg;   // same arch, other variant: last resort
			}
			if ( sub.empty()) sub = fallback;
			if ( sub.empty()) {
				err = "image index has no linux/" + arch_base + ( arch_var.empty() ? "" : "/" + arch_var );
				return false;
			}
			ImageRef r2 = ref;
			r2.digest = sub;   // fetch the selected manifest by digest
			if ( !registry::fetch_manifest(r2, auth_file, body, err)) return false;
			std::string got = "sha256:" + sha256_hex(body);   // the index pointed us at 'sub'; verify
			if ( got != sub ) {
				err = "sub-manifest digest mismatch: expected " + sub + ", got " + got;
				return false;
			}
			m = JSON::parse(body);
		}

		if ( !m.contains("config") || !m.contains("layers")) {
			err = "not an image manifest (missing config/layers)";
			return false;
		}
		out.manifest_json = body;   // the selected single-platform image manifest
		JSON cfg = m["config"];
		out.config_digest = cfg.contains("digest") ? cfg["digest"].to_string() : "";

		JSON ls = m["layers"];
		for ( auto it = ls.begin(); it != ls.end(); ++it ) {
			JSON e = *it.value();
			Layer L;
			L.digest     = e.contains("digest")    ? e["digest"].to_string()    : "";
			L.media_type = e.contains("mediaType") ? e["mediaType"].to_string() : "";
			// declared blob size - what the disk preflight budgets against
			if ( e.contains("size")) L.size = e["size"].to_number();
			if ( !L.digest.empty()) out.layers.push_back(L);
		}
	} catch ( const std::exception& ex ) {
		err = std::string("manifest parse error: ") + ex.what();
		return false;
	}

	if ( out.config_digest.empty()) { err = "manifest has no config digest"; return false; }
	if ( out.layers.empty()) { err = "manifest has no layers"; return false; }
	return true;
}

}
