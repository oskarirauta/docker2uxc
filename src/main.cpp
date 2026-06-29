#include <iostream>
#include <string>
#include <vector>

#include "usage.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "http.hpp"
#include "registry.hpp"
#include "sha256.hpp"

#define D2U_VERSION "0.2.0-dev"

int main(int argc, char** argv) {

	usage_t usage = {
		.args = { argc, argv },
		.info = {
			.name = "docker2uxc",
			.version_title = "version ",
			.version = D2U_VERSION,
			.copyright = "2026, Oskari Rauta",
			.usage = "[options] <image-ref>",
			.description = "\nOCI image -> ujail/uxc bundle converter (C++)",
		},
		.options = {
			{ "out",            { .key = "o", .word = "out",            .desc = "output bundle directory",                  .flag = usage_t::REQUIRED, .name = "dir" }},
			{ "name",           { .key = "n", .word = "name",           .desc = "container name",                            .flag = usage_t::REQUIRED, .name = "name" }},
			{ "arch",           { .key = "a", .word = "arch",           .desc = "target architecture (default: host)",      .flag = usage_t::REQUIRED, .name = "arch" }},
			{ "auth-file",      {             .word = "auth-file",      .desc = "registry credentials (Docker auths JSON)", .flag = usage_t::REQUIRED, .name = "file" }},
			{ "resolve-digest", {             .word = "resolve-digest", .desc = "print the digest the ref resolves to, then exit" }},
			{ "check-updates",  {             .word = "check-updates",  .desc = "report which registered containers have updates" }},
			{ "force",          { .key = "f", .word = "force",          .desc = "overwrite an existing output bundle" }},
			{ "verbose",        { .key = "v", .word = "verbose",        .desc = "verbose / debug logging" }},
			{ "help",           { .key = "h", .word = "help",           .desc = "show this help" }},
			{ "version",        { .key = "V", .word = "version",        .desc = "show version" }},
		}
	};

	if ( (bool)usage["version"] ) { std::cout << usage.version() << std::endl; return 0; }
	if ( (bool)usage["help"] )    { std::cout << usage << "\n" << usage.help() << std::endl; return 0; }
	if ( (bool)usage["verbose"] ) logger::loglevel(logger::debug);

	std::vector<std::string> rest = usage.remainder();
	if ( rest.empty()) {
		logger::error << "docker2uxc: missing <image-ref>" << std::endl;
		std::cout << usage << std::endl;
		return 1;
	}

	ImageRef ref = parse_ref(rest[0]);
	logger::info << "image:   " << ref.reg << "/" << ref.repo
	             << (ref.digest.empty() ? (":" + ref.tag) : ("@" + ref.digest)) << std::endl;
	logger::verbose << "apihost: " << ref.apihost << "  refdesc: " << ref.refdesc() << std::endl;

	std::string auth_file = (bool)usage["auth-file"] ? usage["auth-file"].value : "/etc/uxcd/auth.json";
	http::global_init();

	// --resolve-digest: print sha256 of the resolved manifest (uxcd's update check)
	if ( (bool)usage["resolve-digest"] ) {
		std::string body, err;
		if ( !registry::fetch_manifest(ref, auth_file, body, err)) {
			logger::error << "docker2uxc: " << err << std::endl;
			http::global_cleanup();
			return 1;
		}
		std::cout << "sha256:" << sha256_hex(body) << std::endl;   // stdout = just the digest
		http::global_cleanup();
		return 0;
	}

	logger::error << "docker2uxc: pull/build not wired yet (next increment)" << std::endl;
	http::global_cleanup();
	return 1;
}
