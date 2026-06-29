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

// Ping /v2/; if it 401s, obtain a pull-scoped token (Basic creds when present)
// and return the Authorization header(s) to attach. Empty for an open registry.
static bool authenticate(const ImageRef& ref, const std::string& auth_file,
                         std::vector<std::string>& auth, std::string& err) {
	http::Response ping;
	if ( !http::get("https://" + ref.apihost + "/v2/", {}, ping, err)) return false;
	if ( ping.status == 200 ) { auth.clear(); return true; }
	if ( ping.status != 401 ) { err = "registry /v2/ HTTP " + std::to_string(ping.status); return false; }

	std::string wa = ping.header("www-authenticate");
	std::string realm = www_attr(wa, "realm");
	std::string service = www_attr(wa, "service");
	if ( realm.empty()) { err = "registry 401 without a Bearer realm"; return false; }
	std::string scope = "repository:" + ref.repo + ":pull";

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

	if ( !token.empty()) auth = { "Authorization: Bearer " + token };
	else if ( !creds.empty()) auth = { "Authorization: Basic " + creds };   // pure-Basic registry
	else { err = "could not authenticate to " + ref.reg + " (private image, or wrong/missing credentials)"; return false; }
	return true;
}

bool fetch_manifest(const ImageRef& ref, const std::string& auth_file, std::string& body, std::string& err) {
	std::vector<std::string> headers;
	if ( !authenticate(ref, auth_file, headers, err)) return false;
	headers.push_back(ACCEPT_MANIFEST);
	std::string url = "https://" + ref.apihost + "/v2/" + ref.repo + "/manifests/" + ref.refdesc();
	http::Response r;
	if ( !http::get(url, headers, r, err)) return false;
	if ( r.status != 200 ) { err = "manifest HTTP " + std::to_string(r.status); return false; }
	body = std::move(r.body);
	return true;
}

bool fetch_blob(const ImageRef& ref, const std::string& digest, const std::string& auth_file,
                const std::string& outfile, std::string& err) {
	std::vector<std::string> headers;
	if ( !authenticate(ref, auth_file, headers, err)) return false;
	std::string url = "https://" + ref.apihost + "/v2/" + ref.repo + "/blobs/" + digest;
	return http::get_to_file(url, headers, outfile, err);
}

}
