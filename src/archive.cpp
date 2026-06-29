#include "archive.hpp"
#include "registry.hpp"
#include "sha256.hpp"

namespace archive {

bool download_verify(const ImageRef& ref, const std::string& digest, const std::string& auth_file,
                     const std::string& outfile, std::string& err) {
	if ( !registry::fetch_blob(ref, digest, auth_file, outfile, err)) return false;

	std::string got = sha256_file(outfile, err);
	if ( got.empty()) return false;

	std::string want = digest;
	std::string::size_type c = want.find(':');
	if ( c != std::string::npos ) want = want.substr(c + 1);   // strip the "sha256:" algo prefix

	if ( got != want ) {
		err = "digest mismatch (got sha256:" + got + ", want " + digest + ")";
		return false;
	}
	return true;
}

}
