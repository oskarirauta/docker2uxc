#include "ref.hpp"

// A first path component is the registry only if it looks like a host: it
// contains a '.', a ':' (port), or is exactly "localhost". Otherwise the ref is
// a Docker Hub shorthand and the registry defaults to docker.io.
static bool looks_like_host(const std::string& s) {
	return s == "localhost" ||
	       s.find('.') != std::string::npos ||
	       s.find(':') != std::string::npos;
}

ImageRef parse_ref(const std::string& in) {
	ImageRef r;
	std::string rest = in;

	// digest (@sha256:...)
	std::string::size_type at = rest.find('@');
	if ( at != std::string::npos ) {
		r.digest = rest.substr(at + 1);
		rest = rest.substr(0, at);
	}

	// registry = first path component, only if it looks like a host
	std::string::size_type slash = rest.find('/');
	if ( slash != std::string::npos && looks_like_host(rest.substr(0, slash))) {
		r.reg = rest.substr(0, slash);
		rest = rest.substr(slash + 1);
	} else {
		r.reg = "docker.io";
	}

	// tag = ':' in the LAST path component (the registry port was already removed)
	std::string::size_type lastslash = rest.rfind('/');
	std::string last = ( lastslash == std::string::npos ) ? rest : rest.substr(lastslash + 1);
	std::string::size_type colon = last.rfind(':');
	if ( colon != std::string::npos ) {
		r.tag = last.substr(colon + 1);
		std::string newlast = last.substr(0, colon);
		rest = ( lastslash == std::string::npos ) ? newlast : rest.substr(0, lastslash + 1) + newlast;
	}

	r.repo = rest;
	if ( r.tag.empty()) r.tag = "latest";

	// docker.io: official images live under library/, API host differs
	if ( r.reg == "docker.io" ) {
		if ( r.repo.find('/') == std::string::npos ) r.repo = "library/" + r.repo;
		r.apihost = "registry-1.docker.io";
	} else {
		r.apihost = r.reg;
	}
	return r;
}
