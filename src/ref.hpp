#pragma once
#include <string>

// A parsed image reference: [registry/]repository[:tag][@digest].
struct ImageRef {
	std::string reg;      // registry host (docker.io, ghcr.io, ...)
	std::string repo;     // repository path (docker.io single-segment -> library/<x>)
	std::string tag;      // tag, defaults to "latest"
	std::string digest;   // sha256:... when pinned, else empty
	std::string apihost;  // registry v2 API host (docker.io -> registry-1.docker.io)

	// what the manifest is fetched by: the digest if pinned, else the tag
	std::string refdesc() const { return digest.empty() ? tag : digest; }
};

// Parse a reference exactly like the original shell did.
ImageRef parse_ref(const std::string& s);
