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
#include <sys/wait.h>
#include <unistd.h>
#include <ftw.h>
#include <cerrno>
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

// cp -a <src>/. <dst>/ : clone a whole rootfs tree (used for FROM <earlier-stage>).
// FTW_MOUNT in rm_rf protects removal; here the source stage's binds are already
// torn down (its apply_stage returned), so we only ever copy plain files.
bool copy_tree(const std::string& src, const std::string& dst, std::string& err) {
	pid_t pid = fork();
	if ( pid < 0 ) { err = "fork failed"; return false; }
	if ( pid == 0 ) {
		execlp("cp", "cp", "-a", ( src + "/." ).c_str(), ( dst + "/" ).c_str(), (char*)nullptr);
		_exit(127);
	}
	int st = 0;
	while ( waitpid(pid, &st, 0) < 0 && errno == EINTR ) {}
	if ( WIFEXITED(st) && WEXITSTATUS(st) == 0 ) return true;
	err = "cannot clone base stage rootfs (cp -a failed)";
	return false;
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

// Resolve a manifest, fetch its config blob into cfg_path, and extract every layer
// into rootfs_dir. Shared by a plain pull and each image-based Dockerfile stage.
bool pull_into(const ImageRef& ref, const std::string& arch_base, const std::string& arch_var,
               const std::string& rootfs_dir, const std::string& cfg_path,
               work::Dir& wd, const Options& o, manifest::Image& img, std::string& err) {
	if ( !manifest::resolve(ref, arch_base, arch_var, o.auth_file, img, err)) return false;
	logger::verbose << "  " << img.layers.size() << " layer(s), config " << img.config_digest << std::endl;
	if ( !fetch_blob_cached(ref, img.config_digest, o.auth_file, cfg_path, o.verify, o.cache_dir, err)) { err = "config: " + err; return false; }
	int li = 0;
	for ( const auto& L : img.layers ) {
		++li;
		if ( work::cancelled ) { err = "cancelled"; return false; }
		std::string lf = wd.path() + "/layer" + std::to_string(li);
		logger::verbose << "  layer " << li << "/" << img.layers.size() << std::endl;
		if ( !fetch_blob_cached(ref, L.digest, o.auth_file, lf, o.verify, o.cache_dir, err)) { err = "layer " + std::to_string(li) + ": " + err; return false; }
		if ( !extract::layer(lf, rootfs_dir, wd.path() + "/layer.tar", err)) { err = "extract layer " + std::to_string(li) + ": " + err; return false; }
		unlink(lf.c_str());
	}
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

	// target arch (host default), split into base + variant once for all pulls
	std::string arch = o.arch.empty() ? host_arch() : o.arch;
	std::string arch_base = arch, arch_var;
	{
		std::string::size_type s = arch.find('/');
		if ( s != std::string::npos ) { arch_base = arch.substr(0, s); arch_var = arch.substr(s + 1); }
	}
	logger::info << "arch:    linux/" << arch << std::endl;

	// resolve what we're building (stages or a single ref) and its output name
	std::vector<dockerfile::Stage> stages;
	std::string df_ctx;
	ImageRef    pull_ref;                 // pull mode only
	std::string name = o.name;

	if ( df_mode ) {
		struct stat dst;
		if ( stat(o.dockerfile.c_str(), &dst) != 0 ) { err = "dockerfile not found: " + o.dockerfile; return false; }
		if ( S_ISDIR(dst.st_mode)) {              // a directory: build <dir>/Dockerfile
			if ( o.context.empty()) o.context = o.dockerfile;
			o.dockerfile += "/Dockerfile";
			if ( stat(o.dockerfile.c_str(), &dst) != 0 ) { err = "no Dockerfile in directory " + o.context; return false; }
		}
		df_ctx = o.context;
		if ( df_ctx.empty()) {
			std::string::size_type sl = o.dockerfile.find_last_of('/');
			df_ctx = ( sl == std::string::npos ) ? std::string(".") : ( sl == 0 ? std::string("/") : o.dockerfile.substr(0, sl));
		}
		if ( !dockerfile::parse_stages(o.dockerfile, stages, err)) return false;
		logger::info << "dockerfile: " << o.dockerfile << "  (context: " << df_ctx << ", "
		             << stages.size() << ( stages.size() == 1 ? " stage)" : " stages)" ) << std::endl;
		if ( name.empty()) {
			char* crp = realpath(df_ctx.c_str(), nullptr);   // PATH_MAX-safe
			std::string ctx_abs = crp ? std::string(crp) : df_ctx;
			free(crp);
			std::string::size_type cs = ctx_abs.find_last_of('/');
			name = ( cs == std::string::npos ) ? ctx_abs : ctx_abs.substr(cs + 1);
			if ( name.empty()) name = "container";
		}
	} else {
		if ( o.image.empty()) { err = "missing image ref"; return false; }
		pull_ref = parse_ref(o.image);
		logger::info << "image:   " << pull_ref.reg << "/" << pull_ref.repo
		             << ( pull_ref.digest.empty() ? ( ":" + pull_ref.tag ) : ( "@" + pull_ref.digest )) << std::endl;
		if ( name.empty()) {
			std::string::size_type bs = pull_ref.repo.find_last_of('/');
			name = pull_ref.repo.substr(bs == std::string::npos ? 0 : bs + 1);
		}
	}

	std::string out = o.out.empty() ? ( "./" + name ) : o.out;
	struct stat ost;
	bool exists = ( stat(out.c_str(), &ost) == 0 );
	if ( exists && !o.force ) { err = "output exists: " + out + " (use force)"; return false; }
	if ( exists ) { rm_rf(out + ".prev"); rename(out.c_str(), ( out + ".prev" ).c_str()); }   // keep one gen for rollback

	work::Dir wd;
	if ( !wd.ok()) { err = "cannot create work directory"; return false; }
	mkdir(out.c_str(), 0755);
	std::string rootfs = out + "/rootfs";

	// effective base image of the final rootfs (for manifest.json / notes / provenance)
	ImageRef    eff_ref;
	std::string eff_manifest, eff_cfgdigest, eff_prov;
	std::string cfgblob;                  // path to the final image-config blob

	if ( df_mode ) {
		std::vector<std::string> st_rootfs(stages.size()), st_cfg(stages.size());
		std::vector<ImageRef>    st_ref(stages.size());
		std::vector<std::string> st_manifest(stages.size()), st_cfgdigest(stages.size()), st_prov(stages.size());
		std::vector<char>        freed(stages.size(), 0);
		std::vector<int>         last_use = dockerfile::stage_last_use(stages);
		std::vector<dockerfile::BuiltStage> built;

		for ( std::vector<dockerfile::Stage>::size_type i = 0; i < stages.size(); ++i ) {
			if ( work::cancelled ) { err = "cancelled"; return false; }
			bool is_final = ( i + 1 == stages.size());
			std::string sdir = wd.path() + "/stage" + std::to_string(i);
			mkdir(sdir.c_str(), 0755);
			std::string cfg = sdir + "/config";
			std::string rfs = is_final ? rootfs : ( sdir + "/rootfs" );
			mkdir(rfs.c_str(), 0755);

			logger::info << "==> stage " << ( i + 1 ) << "/" << stages.size()
			             << ( stages[i].name.empty() ? std::string() : ( " (" + stages[i].name + ")" ))
			             << ": FROM " << stages[i].base_ref << std::endl;

			if ( stages[i].base_stage >= 0 ) {
				int b = stages[i].base_stage;
				if ( freed[b] ) { err = "stage " + std::to_string(i + 1) + ": base stage rootfs already freed (internal)"; return false; }
				std::string cerr;
				if ( !copy_tree(st_rootfs[b], rfs, cerr)) { err = "stage " + std::to_string(i + 1) + ": " + cerr; return false; }
				if ( !copy_file(st_cfg[b], cfg)) { err = "stage " + std::to_string(i + 1) + ": cannot inherit base config"; return false; }
				st_ref[i] = st_ref[b]; st_manifest[i] = st_manifest[b];
				st_cfgdigest[i] = st_cfgdigest[b]; st_prov[i] = st_prov[b];
			} else {
				ImageRef ref = parse_ref(stages[i].base_ref);
				manifest::Image img;
				if ( !pull_into(ref, arch_base, arch_var, rfs, cfg, wd, o, img, err)) return false;
				st_ref[i] = ref; st_manifest[i] = img.manifest_json;
				st_cfgdigest[i] = img.config_digest; st_prov[i] = img.provenance_digest;
			}

			if ( !dockerfile::apply_stage(stages[i], df_ctx, rfs, cfg, built, err)) {
				err = "stage " + std::to_string(i + 1) + ": " + err; return false;
			}

			st_rootfs[i] = rfs; st_cfg[i] = cfg;
			built.push_back({ stages[i].name, (int)i, rfs });

			// free earlier intermediate stages whose last consumer was this stage
			for ( std::vector<dockerfile::Stage>::size_type j = 0; j < i; ++j ) {
				if ( freed[j] || ( j + 1 == stages.size())) continue;
				if ( last_use[j] <= (int)i ) { rm_rf(st_rootfs[j]); freed[j] = 1; }
			}
		}

		cfgblob       = st_cfg.back();
		eff_ref       = st_ref.back();
		eff_manifest  = st_manifest.back();
		eff_cfgdigest = st_cfgdigest.back();
		eff_prov      = st_prov.back();
		logger::info << "==> build complete (" << stages.size()
		             << ( stages.size() == 1 ? " stage)" : " stages)" ) << std::endl;
	} else {
		mkdir(rootfs.c_str(), 0755);
		cfgblob = wd.path() + "/config";
		manifest::Image img;
		if ( !pull_into(pull_ref, arch_base, arch_var, rootfs, cfgblob, wd, o, img, err)) return false;
		eff_ref = pull_ref; eff_manifest = img.manifest_json;
		eff_cfgdigest = img.config_digest; eff_prov = img.provenance_digest;
		logger::info << "==> rootfs extracted (" << img.layers.size() << " layers)" << std::endl;
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
	write_file_str(out + "/manifest.json", eff_manifest);

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
		ni.source            = eff_ref.reg + "/" + eff_ref.repo + ":" + eff_ref.tag;
		ni.arch              = arch;
		ni.config_digest     = eff_cfgdigest;
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
		std::string prov_image  = df_mode ? std::string() : o.image;
		std::string prov_digest = df_mode ? std::string() : eff_prov;
		// EXPOSE -> web_ports prefill (pull only; a build's EXPOSE is the base image's)
		JSON web_ports = df_mode ? JSON::Array() : emit::web_ports_from_image(out + "/image-config.json");
		std::string stop_sig = df_mode ? std::string() : emit::stop_signal_from_image(out + "/image-config.json");
		if ( !reg::register_container(o.uxc_dir, name, abs_out, prov_image, prov_digest, o.infra, o.autostart, web_ports, stop_sig, err)) { err = "register: " + err; return false; }
		logger::info << "==> registered: " << o.uxc_dir << "/" << name << ".json" << std::endl;
		if ( web_ports.begin() != web_ports.end())
			logger::info << "    web UI port(s) detected from EXPOSE - review/label them in LuCI" << std::endl;
	}

	o.result_name = name;
	o.result_path = abs_out;
	logger::info << "==> done: " << name << std::endl;
	return true;
}

} // namespace docker2uxc
