#pragma once
#include <string>
#include "ref.hpp"

namespace registry {

// Fetch the raw manifest/index document bytes for `ref`. Performs the standard
// request -> 401 -> token -> retry dance, using credentials from `auth_file`
// (Docker config.json "auths" format) when the repo is private. On success,
// `body` holds the exact bytes (its sha256 is the provenance digest). Returns
// false with `err` set on failure.
bool fetch_manifest(const ImageRef& ref, const std::string& auth_file,
                    std::string& body, std::string& err);

}
