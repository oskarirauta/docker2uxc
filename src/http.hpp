#pragma once
#include <string>
#include <vector>

// Thin libcurl wrapper for the registry client. In-process (no exec, no argv
// credential exposure): request headers carry the Bearer/Basic auth.
namespace http {

struct Response {
	long status = 0;                    // HTTP status (set even on 4xx/5xx)
	std::string body;                   // response body (to-buffer requests)
	std::vector<std::string> headers;   // raw response headers (for WWW-Authenticate)

	// value of a response header (case-insensitive name), or "" if absent
	std::string header(const std::string& name) const;
};

void global_init();
void global_cleanup();

// GET into memory. `extra` holds full header lines ("Accept: ...", "Authorization: ...").
// Returns false only on a transport-level error (err set); the HTTP status is in resp.status.
bool get(const std::string& url, const std::vector<std::string>& extra, Response& resp, std::string& err);

// GET streamed to `path` (for blobs). Returns false on transport error or non-2xx (err set).
bool get_to_file(const std::string& url, const std::vector<std::string>& extra, const std::string& path, std::string& err);

}
