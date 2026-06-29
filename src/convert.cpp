#include "convert.hpp"
#include "logger.hpp"
#include "ref.hpp"
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
#include <vector>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
#include <cstdio>
#include <cstdlib>

namespace docker2uxc {
namespace {

int rm_one(const char* p, const struct stat*, int, struct FTW*) { remove(p); return 0; }
// FTW_MOUNT: never cross a mount point - a stale Dockerfile-build bind (e.g. an
// orphaned /proc after a SIGKILL) is left in place rather than recursed into.
void rm_rf(const std::string& p) { nftw(p.c_str(), rm_one, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT); }

bool copy_file(const std::string& src, const std::string& dst) {
	std::ifstream i(src, std::ios::binary);
	std::ofstream o(dst, std::ios::binary);
	if ( !i || !o ) return false;
	o << i.rdbuf();
	return (bool)o;
}

bool write_file_str(const std::string& path, const std::string& data) {
	std::ofstream o(path, std::ios::binary);
	if ( !o ) return false;
	o << data;
	return (bool)o;
}

std::string host_arch() {
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

// Fetch a blob into `outfile` using a content-addressed cache (<cache>/<hex>)
// when cache_dir is set: download (+verify) into a sibling .part, publish it by
// atomic rename, then hardlink it to the work path (zero-copy; copy across fs).
// A cache hit is re-verified (unless verify==false); a corrupt entry is dropped.
bool fetch_blob_cached(const ImageRef& ref, const std::string& digest, const std::string& auth_file,
                       const std::string& outfile, bool verify, const std::string& cache_dir, std::string& err) {
	if ( cache_dir.empty())
		return archive::download_verify(ref, digest, auth_file, outfile, err, verify);

	std::string hex = digest;
	std::string::size_type c = hex.find(':');
	if ( c != std::string::npos ) hex = hex.substr(c + 1);
	std::string cpath = cache_dir + "/" + hex;

	auto place = [&]() -> bool {
		unlink(outfile.c_str());
		if ( link(cpath.c_str(), outfile.c_str()) == 0 ) return true;
		return copy_file(cpath, outfile);
	};

	struct stat st;
	if ( stat(cpath.c_str(), &st) == 0 && st.st_size > 0 ) {       // cache hit
		bool good = !verify;
		if ( verify ) { std::string g = sha256_file(cpath, err); good = ( !g.empty() && g == hex ); }
		if ( good ) {
			if ( place()) { logger::verbose << "  cache hit " << hex << std::endl; return true; }
			err = "cannot place cached blob " + hex; return false;
		}
		unlink(cpath.c_str());      // corrupt / mismatched cache entry: drop and re-fetch
	}

	mkdir(cache_dir.c_str(), 0755);
	std::string part = cpath + ".part";
	if ( !archive::download_verify(ref, digest, auth_file, part, err, verify)) { unlink(part.c_str()); return false; }
	if ( rename(part.c_str(), cpath.c_str()) != 0 ) {             // unexpected (siblings): use the .part directly
		bool okc = copy_file(part, outfile);
		unlink(part.c_str());
		if ( !okc ) { err = "cannot place blob " + hex; return false; }
		return true;
	}
	if ( !place()) { err = "cannot place cached blob " + hex; return false; }
	return true;
}

} // anonymous namespace

std::string resolve_digest(const std::string& image, const std::string& auth_file, std::string& err) {
	ImageRef ref = parse_ref(image);
	std::string body;
	if ( !registry::fetch_manifest(ref, auth_file, body, err)) return "";
	return "sha256:" + sha256_hex(body);
}

bool check_updates(const std::string& uxc_dir, const std::string& auth_file, std::string& out, std::string& err) {
	(void)err;
	DIR* d = opendir(uxc_dir.c_str());
	if ( !d ) return true;   // no registry yet -> nothing to report

	std::vector<std::string> names;
	for ( struct dirent* e; (e = readdir(d)) != nullptr; ) {
		std::string fn = e->d_name;
		if ( fn.size() <= 5 || fn.substr(fn.size() - 5) != ".json" ) continue;
		names.push_back(fn.substr(0, fn.size() - 5));
	}
	closedir(d);
	std::sort(names.begin(), names.end());

	for ( const std::string& n : names ) {
		if ( work::cancelled ) break;
		std::ifstream f(uxc_dir + "/" + n + ".json");
		if ( !f ) continue;
		std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::string image, old;
		try {
			JSON j = JSON::parse(s);
			if ( j.type() != JSON::TYPE::OBJECT ) continue;
			if ( j.contains("image"))  image = j["image"].to_string();
			if ( j.contains("digest")) old   = j["digest"].to_string();
		} catch ( ... ) { continue; }
		if ( image.empty() || old.empty()) continue;

		std::string e2, neu = resolve_digest(image, auth_file, e2);
		if      ( neu.empty()) out += n + "\terror\t\n";
		else if ( neu == old ) out += n + "\tcurrent\t" + neu + "\n";
		else                   out += n + "\tupdate\t"  + neu + "\n";
	}
	return true;
}

bool convert(Options& o, std::string& err) {
	bool df_mode = !o.dockerfile.empty();
	std::string ref_str, df_ctx;

	if ( df_mode ) {
		df_ctx = o.context;
		if ( df_ctx.empty()) {
			std::string::size_type sl = o.dockerfile.find_last_of('/');
			df_ctx = ( sl == std::string::npos ) ? std::string(".") : ( sl == 0 ? std::string("/") : o.dockerfile.substr(0, sl));
		}
		struct stat dst;
		if ( stat(o.dockerfile.c_str(), &dst) != 0 ) { err = "dockerfile not found: " + o.dockerfile; return false; }
		ref_str = dockerfile::parse_from(o.dockerfile, err);
		if ( ref_str.empty()) return false;   // err set by parse_from
		logger::info << "dockerfile: " << o.dockerfile << "  (context: " << df_ctx << ")" << std::endl;
	} else {
		if ( o.image.empty()) { err = "missing image ref"; return false; }
		ref_str = o.image;
	}

	ImageRef ref = parse_ref(ref_str);
	logger::info << "image:   " << ref.reg << "/" << ref.repo
	             << ( ref.digest.empty() ? ( ":" + ref.tag ) : ( "@" + ref.digest )) << std::endl;
	logger::verbose << "apihost: " << ref.apihost << "  refdesc: " << ref.refdesc() << std::endl;

	std::string arch = o.arch.empty() ? host_arch() : o.arch;
	std::string arch_base = arch, arch_var;
	std::string::size_type s = arch.find('/');
	if ( s != std::string::npos ) { arch_base = arch.substr(0, s); arch_var = arch.substr(s + 1); }

	manifest::Image img;
	if ( !manifest::resolve(ref, arch_base, arch_var, o.auth_file, img, err)) return false;
	logger::info << "arch:    linux/" << arch << std::endl;
	logger::info << "config:  " << img.config_digest << std::endl;
	logger::info << "layers:  " << img.layers.size() << std::endl;

	std::string name = o.name;
	if ( name.empty()) {
		if ( df_mode ) {
			char* crp = realpath(df_ctx.c_str(), nullptr);   // PATH_MAX-safe
			std::string ctx_abs = crp ? std::string(crp) : df_ctx;
			free(crp);
			std::string::size_type cs = ctx_abs.find_last_of('/');
			name = ( cs == std::string::npos ) ? ctx_abs : ctx_abs.substr(cs + 1);
			if ( name.empty()) name = "container";
		} else {
			std::string::size_type bs = ref.repo.find_last_of('/');
			name = ref.repo.substr(bs == std::string::npos ? 0 : bs + 1);
		}
	}
	std::string out = o.out.empty() ? ( "./" + name ) : o.out;

	struct stat ost;
	bool exists = ( stat(out.c_str(), &ost) == 0 );
	if ( exists && !o.force ) { err = "output exists: " + out + " (use force)"; return false; }
	if ( exists ) { rm_rf(out + ".prev"); rename(out.c_str(), ( out + ".prev" ).c_str()); }   // keep one gen for rollback

	work::Dir wd;
	if ( !wd.ok()) { err = "cannot create work directory"; return false; }
	std::string rootfs = out + "/rootfs";
	mkdir(out.c_str(), 0755);
	mkdir(rootfs.c_str(), 0755);

	logger::info << "==> " << out << "  (config + " << img.layers.size() << " layers)" << std::endl;
	std::string cfgblob = wd.path() + "/config";
	if ( !fetch_blob_cached(ref, img.config_digest, o.auth_file, cfgblob, o.verify, o.cache_dir, err)) { err = "config: " + err; return false; }

	int li = 0;
	for ( const auto& L : img.layers ) {
		++li;
		if ( work::cancelled ) { err = "cancelled"; return false; }
		std::string lf = wd.path() + "/layer" + std::to_string(li);
		logger::verbose << "  layer " << li << "/" << img.layers.size() << "  download" << std::endl;
		if ( !fetch_blob_cached(ref, L.digest, o.auth_file, lf, o.verify, o.cache_dir, err)) { err = "layer " + std::to_string(li) + ": " + err; return false; }
		logger::verbose << "  layer " << li << "/" << img.layers.size() << "  extract" << std::endl;
		if ( !extract::layer(lf, rootfs, wd.path() + "/layer.tar", err)) { err = "extract layer " + std::to_string(li) + ": " + err; return false; }
		unlink(lf.c_str());
	}
	logger::info << "==> rootfs extracted (" << img.layers.size() << " layers)" << std::endl;

	if ( df_mode ) {
		logger::info << "==> dockerfile build" << std::endl;
		if ( !dockerfile::apply(o.dockerfile, df_ctx, rootfs, cfgblob, err)) { err = "dockerfile: " + err; return false; }
		logger::info << "==> dockerfile build complete" << std::endl;
	}

	bundle::Opts bo;
	bo.name             = name;
	bo.caps             = o.caps;
	bo.nnp              = !o.privileged;
	bo.network_isolated = o.network_isolated;
	bo.resolvconf       = o.resolvconf;
	bo.accounting       = o.accounting;
	bo.rw_overlay       = o.rw_overlay;
	if ( !bundle::write_config(cfgblob, rootfs, bo, out + "/config.json", err)) { err = "config.json: " + err; return false; }

	if ( !o.profile.empty()) {
		logger::info << "==> applying profile: " << o.profile << std::endl;
		if ( !emit::profile(out + "/config.json", emit::profile_dir(), o.profile, err)) return false;
	}

	copy_file(cfgblob, out + "/image-config.json");
	write_file_str(out + "/manifest.json", img.manifest_json);

	char* orp = realpath(out.c_str(), nullptr);   // PATH_MAX-safe
	std::string abs_out = orp ? std::string(orp) : out;
	free(orp);

	bool emit_net = o.network_isolated || o.emit_netconfig;
	if ( emit_net && !emit::netconfig(out, name, o.net_bridge, err)) { err = "netconfig: " + err; return false; }
	if ( o.emit_keeper && !emit::keeper(out, name, abs_out, err)) { err = "keeper: " + err; return false; }
	{
		emit::NotesInfo ni;
		ni.out               = out;
		ni.name              = name;
		ni.version           = VERSION;
		ni.source            = ref.reg + "/" + ref.repo + ":" + ref.tag;
		ni.arch              = arch;
		ni.config_digest     = img.config_digest;
		ni.image_config_path = out + "/image-config.json";
		ni.config_json_path  = out + "/config.json";
		ni.abs_out           = abs_out;
		ni.network           = o.network_isolated ? "isolated" : "host";
		ni.rw_overlay        = o.rw_overlay;
		ni.emit_keeper       = o.emit_keeper;
		std::string nerr;
		if ( !emit::notes(ni, nerr)) logger::error << "notes: " << nerr << std::endl;   // non-fatal
	}
	emit::warn_missing_binds(out + "/config.json");
	logger::info << "==> bundle ready: " << out << std::endl;

	if ( o.do_register ) {
		// a Dockerfile FROM is the base image, not "the image" - no update provenance
		std::string prov_image  = df_mode ? std::string() : ref_str;
		std::string prov_digest = df_mode ? std::string() : img.provenance_digest;
		if ( !reg::register_container(o.uxc_dir, name, abs_out, prov_image, prov_digest, o.infra, o.autostart, err)) { err = "register: " + err; return false; }
		logger::info << "==> registered: " << o.uxc_dir << "/" << name << ".json" << std::endl;
	}

	o.result_name = name;
	o.result_path = abs_out;
	logger::info << "==> done: " << name << std::endl;
	return true;
}

} // namespace docker2uxc
