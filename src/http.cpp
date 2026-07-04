#include "http.hpp"
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <curl/curl.h>
#include "work.hpp"

namespace http {

// in-memory sink with a hard cap: get() is only used for manifests/tokens/the
// /v2/ ping (all small) - never blobs - so bound it to defend against a
// malicious registry streaming an unbounded body into memory.
struct StrSink { std::string* body; size_t cap; bool over; };

static size_t to_string_cb(char* ptr, size_t sz, size_t n, void* ud) {
	StrSink* s = static_cast<StrSink*>(ud);
	size_t add = sz * n;
	if ( s->body->size() + add > s->cap ) { s->over = true; return 0; }   // returning < requested aborts the transfer
	s->body->append(ptr, add);
	return add;
}

static size_t to_file_cb(char* ptr, size_t sz, size_t n, void* ud) {
	return std::fwrite(ptr, sz, n, static_cast<FILE*>(ud)) * sz;
}

static size_t header_cb(char* ptr, size_t sz, size_t n, void* ud) {
	auto* hs = static_cast<std::vector<std::string>*>(ud);
	std::string line(ptr, sz * n);
	while ( !line.empty() && ( line.back() == '\r' || line.back() == '\n' )) line.pop_back();
	if ( !line.empty()) hs->push_back(line);
	return sz * n;
}

// abort the transfer promptly when a cancel (SIGTERM) arrives
static int xfer_cb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	return work::cancelled ? 1 : 0;
}

static curl_slist* build_headers(const std::vector<std::string>& hs) {
	curl_slist* sl = nullptr;
	for ( const auto& h : hs ) sl = curl_slist_append(sl, h.c_str());
	return sl;
}

std::string Response::header(const std::string& name) const {
	std::string want = name;
	std::transform(want.begin(), want.end(), want.begin(), ::tolower);
	for ( const auto& h : headers ) {
		auto colon = h.find(':');
		if ( colon == std::string::npos ) continue;
		std::string k = h.substr(0, colon);
		std::transform(k.begin(), k.end(), k.begin(), ::tolower);
		if ( k != want ) continue;
		size_t v = colon + 1;
		while ( v < h.size() && h[v] == ' ' ) v++;
		return h.substr(v);
	}
	return "";
}

void global_init()    { curl_global_init(CURL_GLOBAL_DEFAULT); }
void global_cleanup() { curl_global_cleanup(); }

bool get(const std::string& url, const std::vector<std::string>& extra, Response& resp, std::string& err) {
	CURL* c = curl_easy_init();
	if ( !c ) { err = "curl_easy_init failed"; return false; }
	curl_slist* sl = build_headers(extra);
	StrSink sink{ &resp.body, 64UL * 1024 * 1024, false };

	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");   // registries redirect blobs; never follow one to http:// (SSRF/downgrade)
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);   // libcurl strips Authorization on cross-origin redirect
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);         // explicit (libcurl default, but security-critical)
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_string_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, &resp.headers);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "docker2uxc/0.2");
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 0L);      // keep body + status on 4xx (auth flow needs the 401)
	if ( sl ) curl_easy_setopt(c, CURLOPT_HTTPHEADER, sl);

	CURLcode rc = curl_easy_perform(c);
	if ( rc == CURLE_OK ) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
	else err = work::cancelled ? "cancelled" : ( sink.over ? "response exceeds size limit" : curl_easy_strerror(rc));

	if ( sl ) curl_slist_free_all(sl);
	curl_easy_cleanup(c);
	return rc == CURLE_OK;
}

bool get_to_file(const std::string& url, const std::vector<std::string>& extra, const std::string& path, std::string& err) {
	FILE* f = std::fopen(path.c_str(), "wb");
	if ( !f ) { err = "cannot open " + path; return false; }
	CURL* c = curl_easy_init();
	if ( !c ) { std::fclose(f); err = "curl_easy_init failed"; return false; }
	curl_slist* sl = build_headers(extra);

	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");   // registries redirect blobs; never follow one to http:// (SSRF/downgrade)
	curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);         // explicit (libcurl default, but security-critical)
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, to_file_cb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "docker2uxc/0.2");
	curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);      // blobs: treat 4xx/5xx as failure
	if ( sl ) curl_easy_setopt(c, CURLOPT_HTTPHEADER, sl);

	CURLcode rc = curl_easy_perform(c);
	if ( rc != CURLE_OK ) err = work::cancelled ? "cancelled" : curl_easy_strerror(rc);

	if ( sl ) curl_slist_free_all(sl);
	curl_easy_cleanup(c);
	std::fclose(f);
	if ( rc != CURLE_OK ) { std::remove(path.c_str()); return false; }
	return true;
}

}
