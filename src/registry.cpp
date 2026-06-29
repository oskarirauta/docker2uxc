#include "registry.hpp"
#include "http.hpp"
#include "json.hpp"
#include "logger.hpp"

#include <fstream>
#include <vector>
#include <iterator>

namespace registry {

static const char* ACCEPT_MANIFEST =
	"Accept: application/vnd.oci.image.index.v1+json, "
	"application/vnd.docker.distribution.manifest.list.v2+json, "
	"application/vnd.oci.image.manifest.v1+json, "
	"application/vnd.docker.distribution.manifest.v2+json";

// minimal base64 (for the Docker "auth" / user:pass credential)
static std::string b64(const std::string& in) {
	static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	int val = 0, bits = -6;
	for ( unsigned char c : in ) {
		val = (val << 8) + c; bits += 8;
		while ( bits >= 0 ) { out += T[(val >> bits) & 0x3f]; bits -= 6; }
	}
	if ( bits > -6 ) out += T[((val << 8) >> (bits + 8)) & 0x3f];
	while ( out.size() % 4 ) out += '=';
	return out;
}

// pull key="value" out of a WWW-Authenticate Bearer header
static std::string www_attr(const std::string& h, const std::string& key) {
	std::string needle = key + "=\"";
	std::string::size_type p = h.find(needle);
	if ( p == std::string::npos ) return "";
	p += needle.size();
	std::string::size_type e = h.find('"', p);
	return e == std::string::npos ? "" : h.substr(p, e - p);
}

// base64 "user:pass" for `reg` from the Docker auths file, or empty
static std::string registry_creds(const std::string& reg, const std::string& auth_file) {
	if ( auth_file.empty()) return "";
	std::ifstream f(auth_file);
	if ( !f ) return "";
	std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	JSON j;
	try { j = JSON::parse(s); } catch ( ... ) { return ""; }
	if ( j.type() != JSON::TYPE::OBJECT || !j.contains("auths")) return "";
	JSON auths = j["auths"];
	if ( auths.type() != JSON::TYPE::OBJECT ) return "";

	std::vector<std::string> keys;
	if ( reg == "docker.io" )
		keys = { "docker.io", "registry-1.docker.io", "index.docker.io", "https://index.docker.io/v1/" };
	else
		keys = { reg };

	for ( const std::string& k : keys ) {
		if ( !auths.contains(k)) continue;
		JSON e = auths[k];
		if ( e.contains("auth") && e["auth"].type() == JSON::TYPE::STRING && !e["auth"].to_string().empty())
			return e["auth"].to_string();
		if ( e.contains("username") && e["username"].type() == JSON::TYPE::STRING ) {
			std::string u = e["username"].to_string();
			std::string p = ( e.contains("password") && e["password"].type() == JSON::TYPE::STRING ) ? e["password"].to_string() : "";
			if ( !u.empty()) return b64(u + ":" + p);
		}
	}
	return "";
}

bool fetch_manifest(const ImageRef& ref, const std::string& auth_file, std::string& body, std::string& err) {
	std::string url = "https://" + ref.apihost + "/v2/" + ref.repo + "/manifests/" + ref.refdesc();
	std::vector<std::string> headers = { ACCEPT_MANIFEST };

	http::Response r;
	if ( !http::get(url, headers, r, err)) return false;
	if ( r.status == 200 ) { body = std::move(r.body); return true; }
	if ( r.status != 401 ) { err = "manifest HTTP " + std::to_string(r.status); return false; }

	// 401: authenticate from the Bearer challenge
	std::string wa = r.header("www-authenticate");
	std::string realm = www_attr(wa, "realm");
	std::string service = www_attr(wa, "service");
	std::string scope = www_attr(wa, "scope");
	if ( scope.empty()) scope = "repository:" + ref.repo + ":pull";
	if ( realm.empty()) { err = "registry 401 without a Bearer realm"; return false; }

	std::string creds = registry_creds(ref.reg, auth_file);
	if ( !creds.empty()) logger::verbose << "registry: using credentials for " << ref.reg << std::endl;

	std::string tokurl = realm + "?service=" + service + "&scope=" + scope;
	std::vector<std::string> th;
	if ( !creds.empty()) th.push_back("Authorization: Basic " + creds);

	http::Response tr;
	if ( !http::get(tokurl, th, tr, err)) return false;
	std::string token;
	if ( tr.status == 200 ) {
		try {
			JSON tj = JSON::parse(tr.body);
			if ( tj.contains("token")) token = tj["token"].to_string();
			else if ( tj.contains("access_token")) token = tj["access_token"].to_string();
		} catch ( ... ) {}
	}

	if ( !token.empty()) {
		headers.push_back("Authorization: Bearer " + token);
	} else if ( !creds.empty()) {
		headers.push_back("Authorization: Basic " + creds);   // pure-Basic registry fallback
	} else {
		err = "could not obtain a registry token (private image, or wrong/missing credentials)";
		return false;
	}

	http::Response r2;
	if ( !http::get(url, headers, r2, err)) return false;
	if ( r2.status == 200 ) { body = std::move(r2.body); return true; }
	err = "manifest HTTP " + std::to_string(r2.status) + " after authentication";
	return false;
}

}
