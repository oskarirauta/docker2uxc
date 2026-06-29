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
#include "extract.hpp"
#include "bundle.hpp"
#include "reg.hpp"
#include <fstream>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
#include <cstdlib>

#define D2U_VERSION "0.2.0-dev"

static int rm_one(const char* p, const struct stat*, int, struct FTW*) { remove(p); return 0; }
static void rm_rf(const std::string& p) { nftw(p.c_str(), rm_one, 16, FTW_DEPTH | FTW_PHYS); }

static bool copy_file(const std::string& src, const std::string& dst) {
	std::ifstream i(src, std::ios::binary);
	std::ofstream o(dst, std::ios::binary);
	if ( !i || !o ) return false;
	o << i.rdbuf();
	return (bool)o;
}

static bool write_file_str(const std::string& path, const std::string& data) {
	std::ofstream o(path, std::ios::binary);
	if ( !o ) return false;
	o << data;
	return (bool)o;
}

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
			{ "infra",          {             .word = "infra",          .desc = "register as a member of shared netns NAME", .flag = usage_t::REQUIRED, .name = "netns" }},
			{ "caps",           {             .word = "caps",           .desc = "capability set: permissive | minimal",      .flag = usage_t::REQUIRED, .name = "set" }},
			{ "network",        {             .word = "network",        .desc = "host | isolated (default host)",            .flag = usage_t::REQUIRED, .name = "mode" }},
			{ "privileged",     {             .word = "privileged",     .desc = "process.noNewPrivileges = false" }},
			{ "resolv-conf",    {             .word = "resolv-conf",    .desc = "bind-mount the host /etc/resolv.conf" }},
			{ "no-accounting",  {             .word = "no-accounting",  .desc = "omit the memory+pids linux.resources block" }},
			{ "rw-overlay",     {             .word = "rw-overlay",     .desc = "tune config for a writable overlay" }},
			{ "autostart",      {             .word = "autostart",      .desc = "register the container to start on boot" }},
			{ "no-register",    {             .word = "no-register",    .desc = "build the bundle only; do not register with uxcd" }},
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

	std::string::size_type bs = ref.repo.find_last_of('/');
	std::string name = (bool)usage["name"] ? usage["name"].value
	                   : ref.repo.substr(bs == std::string::npos ? 0 : bs + 1);
	std::string out = (bool)usage["out"] ? usage["out"].value : "./" + name;
	struct stat ost;
	bool exists = ( stat(out.c_str(), &ost) == 0 );
	if ( exists && !(bool)usage["force"] ) {
		logger::error << "docker2uxc: output exists: " << out << " (use --force)" << std::endl;
		http::global_cleanup(); return 1;
	}
	if ( exists ) { rm_rf(out + ".prev"); rename(out.c_str(), (out + ".prev").c_str()); }   // keep one gen for rollback

	work::Dir wd;
	if ( !wd.ok()) { logger::error << "docker2uxc: cannot create work directory" << std::endl; http::global_cleanup(); return 1; }
	std::string derr;
	std::string rootfs = out + "/rootfs";
	mkdir(out.c_str(), 0755);
	mkdir(rootfs.c_str(), 0755);

	logger::info << "==> " << out << "  (config + " << img.layers.size() << " layers)" << std::endl;
	std::string cfgblob = wd.path() + "/config";
	if ( !archive::download_verify(ref, img.config_digest, auth_file, cfgblob, derr)) {
		logger::error << "docker2uxc: config: " << derr << std::endl; http::global_cleanup(); return 1;
	}
	int li = 0;
	for ( const auto& L : img.layers ) {
		++li;
		if ( work::cancelled ) { logger::error << "docker2uxc: cancelled" << std::endl; http::global_cleanup(); return 1; }
		std::string lf = wd.path() + "/layer" + std::to_string(li);
		logger::verbose << "  layer " << li << "/" << img.layers.size() << "  download" << std::endl;
		if ( !archive::download_verify(ref, L.digest, auth_file, lf, derr)) {
			logger::error << "docker2uxc: layer " << li << ": " << derr << std::endl; http::global_cleanup(); return 1;
		}
		logger::verbose << "  layer " << li << "/" << img.layers.size() << "  extract" << std::endl;
		if ( !extract::layer(lf, rootfs, wd.path() + "/layer.tar", derr)) {
			logger::error << "docker2uxc: extract layer " << li << ": " << derr << std::endl; http::global_cleanup(); return 1;
		}
		unlink(lf.c_str());
	}
	logger::info << "==> rootfs extracted (" << img.layers.size() << " layers)" << std::endl;

	// OCI bundle config.json + the reference copies
	bundle::Opts bo;
	bo.name             = name;
	bo.caps             = (bool)usage["caps"] ? usage["caps"].value : "permissive";
	bo.nnp              = !(bool)usage["privileged"];
	bo.network_isolated = ( (bool)usage["network"] && usage["network"].value == "isolated" );
	bo.resolvconf       = (bool)usage["resolv-conf"];
	bo.accounting       = !(bool)usage["no-accounting"];
	bo.rw_overlay       = (bool)usage["rw-overlay"];
	if ( !bundle::write_config(cfgblob, rootfs, bo, out + "/config.json", derr)) {
		logger::error << "docker2uxc: config.json: " << derr << std::endl; http::global_cleanup(); return 1;
	}
	copy_file(cfgblob, out + "/image-config.json");
	write_file_str(out + "/manifest.json", img.manifest_json);
	logger::info << "==> bundle ready: " << out << std::endl;

	// provenance + registration (so uxcd can detect updates + adopt it)
	if ( !(bool)usage["no-register"] ) {
		char abuf[4096];
		std::string abs_out = realpath(out.c_str(), abuf) ? std::string(abuf) : out;
		const char* ud = getenv("DOCKER2UXC_UXCDIR");
		std::string uxc_dir = ud ? ud : "/etc/uxc";
		std::string infra = (bool)usage["infra"] ? usage["infra"].value : "";
		if ( !reg::register_container(uxc_dir, name, abs_out, rest[0], img.provenance_digest, infra, (bool)usage["autostart"], derr)) {
			logger::error << "docker2uxc: register: " << derr << std::endl; http::global_cleanup(); return 1;
		}
		logger::info << "==> registered: " << uxc_dir << "/" << name << ".json" << std::endl;
	}

	logger::info << "==> done: " << name << std::endl;
	http::global_cleanup();
	return 0;
}
