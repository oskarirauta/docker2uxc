#pragma once
#include <string>
#include <vector>
#include "ref.hpp"

namespace manifest {

struct Layer {
	std::string digest;       // sha256:...
	std::string media_type;   // tar+gzip / tar+zstd / tar / ...
};

struct Image {
	std::string config_digest;     // the OCI image config blob
	std::vector<Layer> layers;     // ordered, bottom-up
};

// Resolve `ref` to a single-platform image: fetch the manifest, and if it is a
// multi-arch index select the linux/<arch_base>[/<arch_var>] entry and fetch
// that manifest, then parse config + layers. Uses auth_file creds for private
// repos. Returns false + err.
bool resolve(const ImageRef& ref, const std::string& arch_base, const std::string& arch_var,
             const std::string& auth_file, Image& out, std::string& err);

}
