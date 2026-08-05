#pragma once
#include <string>
#include <vector>
#include "ref.hpp"

namespace registry {

// Fetch the raw manifest/index document bytes for `ref` (authenticates as
// needed; uses auth_file creds for private repos). On success `body` holds the
// exact bytes (its sha256 is the provenance digest). Returns false + err.
bool fetch_manifest(const ImageRef& ref, const std::string& auth_file,
                    std::string& body, std::string& err);

// Download a blob (config or layer) by digest to `outfile`. Does NOT verify the
// digest - the caller does. Returns false + err on transport/HTTP failure.
bool fetch_blob(const ImageRef& ref, const std::string& digest,
                const std::string& auth_file, const std::string& outfile, std::string& err);

// List the repository's tags (GET /v2/<repo>/tags/list; first page, up to
// 1000 - enough for a "newer version" scan). Returns false + err.
bool fetch_tags(const ImageRef& ref, const std::string& auth_file,
                std::vector<std::string>& tags, std::string& err);

}
