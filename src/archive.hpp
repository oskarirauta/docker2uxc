#pragma once
#include <string>
#include "ref.hpp"

namespace archive {

// Download a blob by digest to `outfile` and verify its sha256 matches `digest`
// ("sha256:..."). Returns false + err on download or digest-mismatch.
bool download_verify(const ImageRef& ref, const std::string& digest, const std::string& auth_file,
                     const std::string& outfile, std::string& err);

}
