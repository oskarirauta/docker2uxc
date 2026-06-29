#include <iostream>
#include <string>
#include <vector>

#include "usage.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "http.hpp"
#include "registry.hpp"
#include "sha256.hpp"
#include "manifest.hpp"
#include "work.hpp"
#include "archive.hpp"
#include <sys/utsname.h>

#define D2U_VERSION "0.2.0-dev"

static std::string host_arch() {
	struct utsname u;
	if ( uname(&u) != 0 ) return "amd64";
	std::string m = u.machine;
	if ( m == "x86_64" )  return "amd64";
	if ( m == "aarch64" ) return "arm64";
	if ( m == "armv7l" )  return "arm/v7";
	if ( m == "armv6l" )  return "arm/v6";
	if ( m == "i386" || m == "i686" ) return "386";
	return m;
}

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
	work::install_signal_handlers();

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

	// default: pull (partial - manifest resolution; download/extract/bundle land next)
	std::string arch = (bool)usage["arch"] ? usage["arch"].value : host_arch();
	std::string arch_base = arch, arch_var;
	std::string::size_type s = arch.find('/');
	if ( s != std::string::npos ) { arch_base = arch.substr(0, s); arch_var = arch.substr(s + 1); }

	manifest::Image img;
	std::string merr;
	if ( !manifest::resolve(ref, arch_base, arch_var, auth_file, img, merr)) {
		logger::error << "docker2uxc: " << merr << std::endl;
		http::global_cleanup();
		return 1;
	}
	logger::info << "arch:    linux/" << arch << std::endl;
	logger::info << "config:  " << img.config_digest << std::endl;
	logger::info << "layers:  " << img.layers.size() << std::endl;

	work::Dir wd;
	if ( !wd.ok()) { logger::error << "docker2uxc: cannot create work directory" << std::endl; http::global_cleanup(); return 1; }
	std::string derr;

	logger::info << "==> downloading config + " << img.layers.size() << " layers" << std::endl;
	if ( !archive::download_verify(ref, img.config_digest, auth_file, wd.path() + "/config", derr)) {
		logger::error << "docker2uxc: config: " << derr << std::endl; http::global_cleanup(); return 1;
	}
	int li = 0;
	for ( const auto& L : img.layers ) {
		++li;
		if ( work::cancelled ) { logger::error << "docker2uxc: cancelled" << std::endl; http::global_cleanup(); return 1; }
		logger::verbose << "  layer " << li << "/" << img.layers.size() << "  " << L.digest.substr(0, 19) << ".." << std::endl;
		if ( !archive::download_verify(ref, L.digest, auth_file, wd.path() + "/layer" + std::to_string(li), derr)) {
			logger::error << "docker2uxc: layer " << li << ": " << derr << std::endl; http::global_cleanup(); return 1;
		}
	}
	logger::info << "==> all blobs downloaded and sha256-verified" << std::endl;

	logger::error << "docker2uxc: decompress + secure extract + bundle - next increment" << std::endl;
	http::global_cleanup();
	return 1;
}
