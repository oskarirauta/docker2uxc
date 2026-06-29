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
#include "dockerfile.hpp"
#include "emit.hpp"
#include "json.hpp"
#include <fstream>
#include <iterator>
#include <algorithm>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
#include <cstdlib>

#define D2U_VERSION "0.2.0-dev"

static int rm_one(const char* p, const struct stat*, int, struct FTW*) { remove(p); return 0; }
// FTW_MOUNT: never cross a mount point - a stale Dockerfile-build bind (e.g. an
// orphaned /proc after a SIGKILL) is left in place rather than recursed into.
static void rm_rf(const std::string& p) { nftw(p.c_str(), rm_one, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT); }

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

static std::string uxc_registry_dir() {
	const char* ud = getenv("DOCKER2UXC_UXCDIR");
	return ud ? ud : "/etc/uxc";
}

// --check-updates: for each registered container that carries image+digest,
// re-resolve the upstream digest and print one line per container:
//   <name>\t<current|update|error>\t<digest>
// Mirrors the shell exactly (uxcd parses this), but resolves in-process - no
// per-container self-exec of --resolve-digest.
static int check_updates(const std::string& auth_file) {
	std::string dir = uxc_registry_dir();
	DIR* d = opendir(dir.c_str());
	if ( !d ) return 0;   // no registry yet -> nothing to report (shell: empty glob)

	std::vector<std::string> names;
	for ( struct dirent* e; (e = readdir(d)) != nullptr; ) {
		std::string fn = e->d_name;
		if ( fn.size() <= 5 || fn.substr(fn.size() - 5) != ".json" ) continue;
		names.push_back(fn.substr(0, fn.size() - 5));
	}
	closedir(d);
	std::sort(names.begin(), names.end());   // match the shell's sorted glob order

	for ( const std::string& n : names ) {
		if ( work::cancelled ) break;
		std::ifstream f(dir + "/" + n + ".json");
		if ( !f ) continue;
		std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::string image, old;
		try {
			JSON j = JSON::parse(s);
			if ( j.type() != JSON::TYPE::OBJECT ) continue;
			if ( j.contains("image"))  image = j["image"].to_string();
			if ( j.contains("digest")) old   = j["digest"].to_string();
		} catch ( ... ) { continue; }
		if ( image.empty() || old.empty()) continue;   // only registry-pulled images

		ImageRef r = parse_ref(image);
		std::string body, err, neu;
		if ( registry::fetch_manifest(r, auth_file, body, err))
			neu = "sha256:" + sha256_hex(body);

		if      ( neu.empty()) std::cout << n << "\terror\t"          << "\n";
		else if ( neu == old ) std::cout << n << "\tcurrent\t" << neu << "\n";
		else                   std::cout << n << "\tupdate\t"  << neu << "\n";
		std::cout.flush();
	}
	return 0;
}

// Fetch a blob into `outfile`, using a content-addressed cache (<cache>/<hex>,
// the shell's layout) when cache_dir is set. Cache hits are re-verified (unless
// --no-verify); a fresh download populates the cache. Returns false + err.
static bool fetch_blob_cached(const ImageRef& ref, const std::string& digest, const std::string& auth_file,
                              const std::string& outfile, bool verify, const std::string& cache_dir, std::string& err) {
	std::string hex = digest;
	std::string::size_type c = hex.find(':');
	if ( c != std::string::npos ) hex = hex.substr(c + 1);
	std::string cpath = cache_dir.empty() ? std::string() : ( cache_dir + "/" + hex );

	if ( !cpath.empty()) {                            // cache hit?
		struct stat st;
		if ( stat(cpath.c_str(), &st) == 0 && st.st_size > 0 ) {
			bool good = true;
			if ( verify ) { std::string g = sha256_file(cpath, err); good = ( !g.empty() && g == hex ); }
			if ( good && copy_file(cpath, outfile)) { logger::verbose << "  cache hit " << hex << std::endl; return true; }
		}
	}
	if ( !archive::download_verify(ref, digest, auth_file, outfile, err, verify)) return false;
	if ( !cpath.empty()) { mkdir(cache_dir.c_str(), 0755); copy_file(outfile, cpath); }   // populate cache
	return true;
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
			{ "dockerfile",     { .key = "d", .word = "dockerfile",     .desc = "build from a Dockerfile (FROM = base image)", .flag = usage_t::REQUIRED, .name = "file" }},
			{ "context",        {             .word = "context",        .desc = "build context for COPY/ADD (default: Dockerfile dir)", .flag = usage_t::REQUIRED, .name = "dir" }},
			{ "auth-file",      {             .word = "auth-file",      .desc = "registry credentials (Docker auths JSON)", .flag = usage_t::REQUIRED, .name = "file" }},
			{ "infra",          {             .word = "infra",          .desc = "register as a member of shared netns NAME", .flag = usage_t::REQUIRED, .name = "netns" }},
			{ "caps",           {             .word = "caps",           .desc = "capability set: permissive | minimal",      .flag = usage_t::REQUIRED, .name = "set" }},
			{ "profile",        {             .word = "profile",        .desc = "apply profiles/<NAME>.json overlay",        .flag = usage_t::REQUIRED, .name = "name" }},
			{ "network",        {             .word = "network",        .desc = "host | isolated (default host)",            .flag = usage_t::REQUIRED, .name = "mode" }},
			{ "emit-netconfig", {             .word = "emit-netconfig", .desc = "write an /etc/config/network veth/infra snippet" }},
			{ "net-bridge",     {             .word = "net-bridge",     .desc = "bridge for --emit-netconfig (default br-lan)", .flag = usage_t::REQUIRED, .name = "br" }},
			{ "emit-keeper",    {             .word = "emit-keeper",    .desc = "write <name>.init: a procd keeper service" }},
			{ "privileged",     {             .word = "privileged",     .desc = "process.noNewPrivileges = false" }},
			{ "resolv-conf",    {             .word = "resolv-conf",    .desc = "bind-mount the host /etc/resolv.conf" }},
			{ "no-accounting",  {             .word = "no-accounting",  .desc = "omit the memory+pids linux.resources block" }},
			{ "no-verify",      {             .word = "no-verify",      .desc = "skip sha256 verification of downloaded blobs" }},
			{ "cache",          {             .word = "cache",          .desc = "blob cache directory (default /tmp/docker2uxc-cache)", .flag = usage_t::REQUIRED, .name = "dir" }},
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

	std::string auth_file = (bool)usage["auth-file"] ? usage["auth-file"].value : "/etc/uxcd/auth.json";
	http::global_init();
	work::install_signal_handlers();

	// --check-updates: no positional ref required; loop the registry and report.
	if ( (bool)usage["check-updates"] ) {
		int rc = check_updates(auth_file);
		http::global_cleanup();
		return rc;
	}

	std::vector<std::string> rest = usage.remainder();

	// In Dockerfile mode the base image comes from FROM, not a positional ref.
	bool df_mode = (bool)usage["dockerfile"];
	std::string df_path, df_ctx, ref_str;
	if ( df_mode ) {
		df_path = usage["dockerfile"].value;
		std::string::size_type sl = df_path.find_last_of('/');
		df_ctx = (bool)usage["context"] ? usage["context"].value
		         : ( sl == std::string::npos ? std::string(".") : ( sl == 0 ? std::string("/") : df_path.substr(0, sl)));
		struct stat dst;
		if ( stat(df_path.c_str(), &dst) != 0 ) {
			logger::error << "docker2uxc: dockerfile not found: " << df_path << std::endl;
			http::global_cleanup(); return 1;
		}
		std::string ferr;
		ref_str = dockerfile::parse_from(df_path, ferr);
		if ( ref_str.empty()) {
			logger::error << "docker2uxc: " << ferr << std::endl;
			http::global_cleanup(); return 1;
		}
		logger::info << "dockerfile: " << df_path << "  (context: " << df_ctx << ")" << std::endl;
	} else {
		if ( rest.empty()) {
			logger::error << "docker2uxc: missing <image-ref>" << std::endl;
			std::cout << usage << std::endl;
			http::global_cleanup(); return 1;
		}
		ref_str = rest[0];
	}

	ImageRef ref = parse_ref(ref_str);
	logger::info << "image:   " << ref.reg << "/" << ref.repo
	             << (ref.digest.empty() ? (":" + ref.tag) : ("@" + ref.digest)) << std::endl;
	logger::verbose << "apihost: " << ref.apihost << "  refdesc: " << ref.refdesc() << std::endl;

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
	std::string name;
	if ( (bool)usage["name"] ) name = usage["name"].value;
	else if ( df_mode ) {   // default to the build-context dir name (the base ref would be wrong)
		char cbuf[4096];
		std::string ctx_abs = realpath(df_ctx.c_str(), cbuf) ? std::string(cbuf) : df_ctx;
		std::string::size_type cs = ctx_abs.find_last_of('/');
		name = ( cs == std::string::npos ) ? ctx_abs : ctx_abs.substr(cs + 1);
		if ( name.empty()) name = "container";
	} else name = ref.repo.substr(bs == std::string::npos ? 0 : bs + 1);
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

	bool verify = !(bool)usage["no-verify"];
	const char* cenv = getenv("DOCKER2UXC_CACHE");
	std::string cache_dir = (bool)usage["cache"] ? usage["cache"].value : ( cenv ? cenv : "/tmp/docker2uxc-cache" );

	logger::info << "==> " << out << "  (config + " << img.layers.size() << " layers)" << std::endl;
	std::string cfgblob = wd.path() + "/config";
	if ( !fetch_blob_cached(ref, img.config_digest, auth_file, cfgblob, verify, cache_dir, derr)) {
		logger::error << "docker2uxc: config: " << derr << std::endl; http::global_cleanup(); return 1;
	}
	int li = 0;
	for ( const auto& L : img.layers ) {
		++li;
		if ( work::cancelled ) { logger::error << "docker2uxc: cancelled" << std::endl; http::global_cleanup(); return 1; }
		std::string lf = wd.path() + "/layer" + std::to_string(li);
		logger::verbose << "  layer " << li << "/" << img.layers.size() << "  download" << std::endl;
		if ( !fetch_blob_cached(ref, L.digest, auth_file, lf, verify, cache_dir, derr)) {
			logger::error << "docker2uxc: layer " << li << ": " << derr << std::endl; http::global_cleanup(); return 1;
		}
		logger::verbose << "  layer " << li << "/" << img.layers.size() << "  extract" << std::endl;
		if ( !extract::layer(lf, rootfs, wd.path() + "/layer.tar", derr)) {
			logger::error << "docker2uxc: extract layer " << li << ": " << derr << std::endl; http::global_cleanup(); return 1;
		}
		unlink(lf.c_str());
	}
	logger::info << "==> rootfs extracted (" << img.layers.size() << " layers)" << std::endl;

	// Dockerfile build: apply RUN/COPY/ENV/... on top of the extracted base rootfs.
	// This mutates cfgblob in place, so the bundle config + image-config.json below
	// reflect the build's ENV/WORKDIR/USER/CMD/ENTRYPOINT.
	if ( df_mode ) {
		logger::info << "==> dockerfile build" << std::endl;
		if ( !dockerfile::apply(df_path, df_ctx, rootfs, cfgblob, derr)) {
			logger::error << "docker2uxc: dockerfile: " << derr << std::endl; http::global_cleanup(); return 1;
		}
		logger::info << "==> dockerfile build complete" << std::endl;
	}

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

	// profile overlay: deep-merge profiles/<name>.json onto config.json
	if ( (bool)usage["profile"] ) {
		std::string pname = usage["profile"].value;
		logger::info << "==> applying profile: " << pname << std::endl;
		if ( !emit::profile(out + "/config.json", emit::profile_dir(), pname, derr)) {
			logger::error << "docker2uxc: " << derr << std::endl; http::global_cleanup(); return 1;
		}
	}

	copy_file(cfgblob, out + "/image-config.json");
	write_file_str(out + "/manifest.json", img.manifest_json);

	char abuf[4096];
	std::string abs_out = realpath(out.c_str(), abuf) ? std::string(abuf) : out;

	// optional bundle-side emitters, then the always-written notes + bind warning
	bool emit_net = bo.network_isolated || (bool)usage["emit-netconfig"];
	if ( emit_net ) {
		std::string bridge = (bool)usage["net-bridge"] ? usage["net-bridge"].value : "br-lan";
		if ( !emit::netconfig(out, name, bridge, derr)) { logger::error << "docker2uxc: netconfig: " << derr << std::endl; http::global_cleanup(); return 1; }
	}
	if ( (bool)usage["emit-keeper"] ) {
		if ( !emit::keeper(out, name, abs_out, derr)) { logger::error << "docker2uxc: keeper: " << derr << std::endl; http::global_cleanup(); return 1; }
	}
	{
		emit::NotesInfo ni;
		ni.out               = out;
		ni.name              = name;
		ni.version           = D2U_VERSION;
		ni.source            = ref.reg + "/" + ref.repo + ":" + ref.tag;
		ni.arch              = arch;
		ni.config_digest     = img.config_digest;
		ni.image_config_path = out + "/image-config.json";
		ni.config_json_path  = out + "/config.json";
		ni.abs_out           = abs_out;
		ni.network           = bo.network_isolated ? "isolated" : "host";
		ni.rw_overlay        = bo.rw_overlay;
		ni.emit_keeper       = (bool)usage["emit-keeper"];
		std::string nerr;
		if ( !emit::notes(ni, nerr)) logger::error << "docker2uxc: notes: " << nerr << std::endl;   // non-fatal
	}
	emit::warn_missing_binds(out + "/config.json");
	logger::info << "==> bundle ready: " << out << std::endl;

	// provenance + registration (so uxcd can detect updates + adopt it)
	if ( !(bool)usage["no-register"] ) {
		const char* ud = getenv("DOCKER2UXC_UXCDIR");
		std::string uxc_dir = ud ? ud : "/etc/uxc";
		std::string infra = (bool)usage["infra"] ? usage["infra"].value : "";
		// a Dockerfile FROM is the base image, not "the image" - no update provenance
		std::string prov_image  = df_mode ? std::string() : ref_str;
		std::string prov_digest = df_mode ? std::string() : img.provenance_digest;
		if ( !reg::register_container(uxc_dir, name, abs_out, prov_image, prov_digest, infra, (bool)usage["autostart"], derr)) {
			logger::error << "docker2uxc: register: " << derr << std::endl; http::global_cleanup(); return 1;
		}
		logger::info << "==> registered: " << uxc_dir << "/" << name << ".json" << std::endl;
	}

	logger::info << "==> done: " << name << std::endl;
	http::global_cleanup();
	return 0;
}
