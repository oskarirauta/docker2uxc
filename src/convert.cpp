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
#include "space.hpp"
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
#include <cctype>
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

// Is this layer blob already in the cache? (Only for budgeting - a hit is
// re-verified when it is actually used.)
bool blob_cached(const std::string& cache_dir, const std::string& digest) {
	if ( cache_dir.empty()) return false;
	std::string hex = digest;
	std::string::size_type c = hex.find(':');
	if ( c != std::string::npos ) hex = hex.substr(c + 1);
	struct stat st;
	return ( stat(( cache_dir + "/" + hex ).c_str(), &st) == 0 && st.st_size > 0 );
}

// Refuse a pull that plainly does not fit, BEFORE a single byte is downloaded.
// Running a filesystem to zero bytes free is not an ordinary error on a small
// box: everything that wants to write - logs, ubus, the shell you would fix it
// from - blocks or dies, and that is what "the whole system froze and then
// crashed" looks like from the outside. Manifests declare each layer's
// compressed size, so the budget is a real number, not a guess.
//
//   cache  <- the compressed blobs we still have to download
//   bundle <- the extracted rootfs, roughly 2.5x compressed for gzip/zstd
//
// When the two live on the same filesystem they are added together. A cache on
// tmpfs is RAM: filling it is worse than filling a disk, so it gets the same
// budget check and a louder message.
bool preflight_space(const manifest::Image& img, const std::string& rootfs_dir,
                     const std::string& work_dir, const Options& o, std::string& err) {
	unsigned long long dl = 0, total = 0, biggest = 0;
	bool sizes_known = true;
	for ( const auto& L : img.layers ) {
		if ( L.size <= 0 ) { sizes_known = false; continue; }
		total += (unsigned long long)L.size;
		if ( (unsigned long long)L.size > biggest ) biggest = (unsigned long long)L.size;
		if ( !blob_cached(o.cache_dir, L.digest)) dl += (unsigned long long)L.size;
	}
	if ( !sizes_known || total == 0 ) return true;      // nothing to budget against

	unsigned long long need_rootfs  = total / 2 * 5;         // ~2.5x expansion
	unsigned long long need_cache   = o.cache_dir.empty() ? 0 : dl;
	unsigned long long need_scratch = biggest / 2 * 7;       // one layer, compressed + decompressed

	space::Info bundle_fs  = space::of(rootfs_dir);
	space::Info cache_fs   = o.cache_dir.empty() ? space::Info() : space::of(o.cache_dir);
	space::Info scratch_fs = space::of(work_dir);

	// fold the budgets of everything that shares a filesystem
	struct Claim { space::Info fs; unsigned long long need; std::string what; };
	std::vector<Claim> claims;
	auto claim = [&](const space::Info& fs, unsigned long long need, const std::string& what) {
		if ( !fs.ok || need == 0 ) return;
		for ( Claim& c : claims )
			if ( space::same(c.fs, fs)) { c.need += need; c.what += " + " + what; return; }
		claims.push_back({ fs, need, what });
	};
	claim(bundle_fs,  need_rootfs,  "unpacked rootfs");
	claim(cache_fs,   need_cache,   "download cache");
	claim(scratch_fs, need_scratch, "scratch");

	for ( const Claim& c : claims ) {
		if ( space::usable(c.fs) >= c.need ) continue;
		err = "not enough space for the " + c.what + " on " + c.fs.mount + ": need " +
		      space::human(c.need) + ", " + space::human(space::usable(c.fs)) + " usable (" +
		      space::human(c.fs.avail) + " free, keeping " + space::human(space::reserve(c.fs)) +
		      " in reserve so the system stays usable)" +
		      ( c.fs.tmpfs ? ". That filesystem is RAM, not disk" : "" ) +
		      ". Free space, or use --out <dir> / --cache <dir> on a filesystem that has room";
		return false;
	}

	if ( cache_fs.ok && cache_fs.tmpfs && need_cache > cache_fs.total / 4 )
		logger::info << "note: the blob cache (" << o.cache_dir << ") is in RAM and this image needs "
		             << space::human(need_cache) << " of it - use --cache <dir on disk> if memory is tight" << std::endl;
	logger::info << "size:    " << space::human(total) << " to download, about "
	             << space::human(need_rootfs) << " unpacked" << std::endl;
	return true;
}

// Resolve a manifest, fetch its config blob into cfg_path, and extract every layer
// into rootfs_dir. Shared by a plain pull and each image-based Dockerfile stage.
bool pull_into(const ImageRef& ref, const std::string& arch_base, const std::string& arch_var,
               const std::string& rootfs_dir, const std::string& cfg_path,
               work::Dir& wd, const Options& o, manifest::Image& img, std::string& err) {
	if ( !manifest::resolve(ref, arch_base, arch_var, o.auth_file, img, err)) return false;
	logger::verbose << "  " << img.layers.size() << " layer(s), config " << img.config_digest << std::endl;
	if ( !preflight_space(img, rootfs_dir, wd.path(), o, err)) return false;
	if ( !fetch_blob_cached(ref, img.config_digest, o.auth_file, cfg_path, o.verify, o.cache_dir, err)) { err = "config: " + err; return false; }
	int li = 0;
	for ( const auto& L : img.layers ) {
		++li;
		if ( work::cancelled ) { err = "cancelled"; return false; }
		std::string lf = wd.path() + "/layer" + std::to_string(li);
		logger::verbose << "  layer " << li << "/" << img.layers.size() << std::endl;
		// watch the filesystem we are writing to and stop while there is still
		// room to recover in - a download lands in the cache, the extraction in
		// the bundle, and those can be different filesystems
		space::arm(o.cache_dir.empty() ? wd.path() : o.cache_dir);
		bool got = fetch_blob_cached(ref, L.digest, o.auth_file, lf, o.verify, o.cache_dir, err);
		space::disarm();
		if ( !got ) { err = "layer " + std::to_string(li) + ": " + err; return false; }
		space::arm(rootfs_dir);
		bool unpacked = extract::layer(lf, rootfs_dir, wd.path() + "/layer.tar", err);
		space::disarm();
		if ( !unpacked ) { err = "extract layer " + std::to_string(li) + ": " + err; return false; }
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

// ---- "newer version tag" heuristic ------------------------------------------
// Parse a tag into (numeric core, family, prerelease). Returns false for tags
// with no leading version number (latest, stable, mainline-alpine, ...).
// family "" = a plain version; an alpha/beta/rc/dev suffix is a prerelease of
// the plain family; ANY other suffix (e.g. "-alpine", "-tensorrt") is its own
// variant family. Candidates must match the current tag's family, so
// nginx:1.29-alpine is only ever offered *-alpine tags and frigate:0.17.2
// never sees -tensorrt builds.
struct TagVer {
	std::vector<long> num;      // 0.17.2 -> {0,17,2}
	std::string family;         // "" plain, else the variant suffix (e.g. "-alpine")
	bool pre = false;           // prerelease of the plain family
	int  pre_rank = 0;          // alpha/dev/pre=0  beta=1  rc=2
	long pre_num  = 0;          // beta2 -> 2
};
static bool parse_tagver(const std::string& tag, TagVer& v) {
	std::string t = tag;
	if ( !t.empty() && ( t[0] == 'v' || t[0] == 'V' )) t = t.substr(1);
	std::string::size_type i = 0;
	while ( i < t.size() && isdigit((unsigned char)t[i])) {
		std::string::size_type j = i;
		while ( j < t.size() && isdigit((unsigned char)t[j])) j++;
		v.num.push_back(atol(t.substr(i, j - i).c_str()));
		i = j;
		if ( i + 1 < t.size() && t[i] == '.' && isdigit((unsigned char)t[i + 1])) i++;
		else break;
	}
	if ( v.num.empty()) return false;
	// an all-digit commit sha ("9950684") is not a version: accept a
	// single-component number only when it is small (postgres:16 style)
	if ( v.num.size() == 1 && v.num[0] > 9999 ) return false;
	std::string rest = t.substr(i);
	if ( rest.empty()) return true;                    // plain 0.17.2
	std::string low;
	for ( char c : rest ) low += (char)tolower((unsigned char)c);
	std::string::size_type s = ( low[0] == '-' || low[0] == '.' ) ? 1 : 0;
	static const struct { const char* w; int rank; } P[] =
		{{ "alpha", 0 }, { "beta", 1 }, { "rc", 2 }, { "dev", 0 }, { "pre", 0 }};
	for ( const auto& p : P ) {
		std::string w = p.w;
		if ( low.compare(s, w.size(), w) == 0 ) {
			std::string tail = low.substr(s + w.size());
			if ( !tail.empty() && ( tail[0] == '.' || tail[0] == '-' )) tail = tail.substr(1);
			if ( tail.find_first_not_of("0123456789") == std::string::npos ) {
				v.pre = true; v.pre_rank = p.rank;
				v.pre_num = tail.empty() ? 0 : atol(tail.c_str());
				return true;
			}
		}
	}
	v.family = rest;                                   // variant (-alpine, -tensorrt, ...)
	return true;
}
static int cmp_num(const std::vector<long>& a, const std::vector<long>& b) {
	std::vector<long>::size_type n = a.size() > b.size() ? a.size() : b.size();
	for ( std::vector<long>::size_type i = 0; i < n; i++ ) {
		long x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
		if ( x != y ) return x < y ? -1 : 1;
	}
	return 0;
}
static bool tag_newer(const TagVer& a, const TagVer& b) {   // b newer than a? (same family)
	int c = cmp_num(a.num, b.num);
	if ( c != 0 ) return c < 0;
	if ( a.pre != b.pre ) return a.pre && !b.pre;       // same core: a stable beats its prereleases
	if ( !a.pre ) return false;
	if ( a.pre_rank != b.pre_rank ) return a.pre_rank < b.pre_rank;
	return a.pre_num < b.pre_num;
}
// The best upgrade-candidate tag newer than `cur`: a stable version if any,
// else the newest prerelease (frigate publishes betas long before a release).
// "" when there is nothing newer or `cur` is not version-like.
static std::string newest_tag(const std::string& cur, const std::vector<std::string>& tags) {
	TagVer c;
	if ( !parse_tagver(cur, c)) return "";
	std::string best_stable, best_pre;
	TagVer bs = c, bp = c;
	for ( const std::string& t : tags ) {
		TagVer v;
		if ( !parse_tagver(t, v)) continue;
		if ( v.family != c.family ) continue;
		if ( v.num.size() == 1 && c.num.size() > 1 ) continue;   // "1234" sha-ish vs a dotted version
		if ( v.pre ) { if ( c.family.empty() && tag_newer(bp, v)) { bp = v; best_pre = t; } }
		else         { if ( tag_newer(bs, v)) { bs = v; best_stable = t; } }
	}
	return !best_stable.empty() ? best_stable : best_pre;
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

		// also scan the repo's tags for a newer VERSION (a tag the recorded one
		// can never "move" to by itself) - reported as an optional 4th column
		std::string newer;
		{
			ImageRef r = parse_ref(image);
			if ( r.digest.empty() && !r.tag.empty()) {   // a digest-pinned ref has no tag to follow
				std::vector<std::string> tags;
				std::string e3;
				if ( registry::fetch_tags(r, auth_file, tags, e3))
					newer = newest_tag(r.tag, tags);
			}
		}
		std::string extra = newer.empty() ? std::string() : ( "\t" + newer );

		if      ( neu.empty()) out += n + "\terror\t\n";
		else if ( neu == old ) out += n + "\tcurrent\t" + neu + extra + "\n";
		else                   out += n + "\tupdate\t"  + neu + extra + "\n";
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
	// build into <out>.new and swap into place only when the bundle is COMPLETE:
	// a cancelled or failed pull must never eat the live bundle or its .prev
	// backup - they are the rollback safety net for an upgrade re-pull.
	std::string out_final = out;
	out = out_final + ".new";
	rm_rf(out);                          // stale leftovers from an earlier
	rm_rf(out_final + ".prev.old");      // cancelled/failed/killed run

	// Say where this is going BEFORE downloading a gigabyte to the wrong place:
	// with no --out a bundle lands in ./<name>, i.e. the current directory.
	{
		std::string parent = out_final.substr(0, out_final.find_last_of('/') == std::string::npos ? 0 : out_final.find_last_of('/'));
		char* prp = realpath(parent.empty() ? "." : parent.c_str(), nullptr);
		std::string pabs = prp ? std::string(prp) : parent;
		free(prp);
		std::string base = out_final.substr(out_final.find_last_of('/') == std::string::npos ? 0 : out_final.find_last_of('/') + 1);
		logger::info << "output:  " << ( pabs.empty() ? "" : pabs + "/" ) << base
		             << ( o.out.empty() ? "   (default location - set it with --out)" : "" ) << std::endl;
	}

	// A cancelled or failed run must not leave a half-extracted rootfs sitting on
	// the disk it just filled: that is the difference between "the pull failed"
	// and "the pull failed AND the partition is still full".
	struct OutCleanup {
		std::string path;
		bool armed = true;
		~OutCleanup() { if ( armed ) rm_rf(path); }
	} out_cleanup{ out };

	// Scratch holds one fully decompressed layer at a time. /tmp is RAM on
	// OpenWrt, so prefer the bundle's own storage when it is real disk - an
	// image that fits on /srv should never be able to exhaust memory.
	std::string work_base;
	{
		const char* e = getenv("DOCKER2UXC_WORK");
		if ( e && *e ) work_base = e;
		else {
			std::string parent = out_final.substr(0, out_final.find_last_of('/'));
			if ( out_final.find_last_of('/') == std::string::npos ) parent = ".";
			space::Info ofs = space::of(parent), tfs = space::of("/tmp");
			if ( ofs.ok && !ofs.tmpfs && ( !tfs.ok || tfs.tmpfs || space::usable(ofs) > space::usable(tfs)))
				work_base = parent;
		}
	}
	work::Dir wd(work_base);
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
	if ( o.dev ) {
		// dev container: run an idle cntrinit as PID 1 (no child) so a daemonless
		// image stays "running" and you shell in with `uxe`. Copy the static init
		// into the bundle rootfs; the trailing "--" puts cntrinit in idle mode.
		std::string cdst = rootfs + "/.cntrinit";
		if ( !copy_file(o.cntrinit, cdst)) { err = "dev: cannot copy cntrinit from " + o.cntrinit + " (is the cntrinit package installed?)"; return false; }
		chmod(cdst.c_str(), 0755);
		bo.args_override = { "/.cntrinit", "--" };
	}
	if ( !bundle::write_config(cfgblob, rootfs, bo, out + "/config.json", err)) { err = "config.json: " + err; return false; }

	emit::ProfileInfo pinfo;
	if ( !o.profile.empty()) {
		logger::info << "==> applying profile: " << o.profile << std::endl;
		if ( !emit::profile(out + "/config.json", emit::profile_dir(), o.profile, &pinfo, err)) return false;
	}

	copy_file(cfgblob, out + "/image-config.json");
	write_file_str(out + "/manifest.json", eff_manifest);

	// the new bundle is complete - now (and only now) rotate: live -> .prev,
	// new -> live. Renames first (fast, near-atomic), the slow delete of the
	// old backup LAST under a junk name - so a cancel, kill or power-off at
	// any instant leaves a consistent live/.prev pair (stray .prev.old/.new
	// are cleared by the next run).
	if ( work::cancelled ) { err = "cancelled"; return false; }
	if ( exists ) {
		rename(( out_final + ".prev" ).c_str(), ( out_final + ".prev.old" ).c_str());   // may fail: no .prev
		if ( rename(out_final.c_str(), ( out_final + ".prev" ).c_str()) != 0 ) { err = "cannot rotate " + out_final + " to .prev"; return false; }
	}
	if ( rename(out.c_str(), out_final.c_str()) != 0 ) { err = "cannot move " + out + " into place"; return false; }
	out_cleanup.armed = false;           // the bundle is live now, not scratch
	out = out_final;
	rm_rf(out_final + ".prev.old");

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
	logger::info << "==> bundle ready: " << abs_out << std::endl;

	if ( o.do_register ) {
		// a Dockerfile FROM is the base image, not "the image" - no update provenance
		std::string prov_image  = df_mode ? std::string() : o.image;
		std::string prov_digest = df_mode ? std::string() : eff_prov;
		// EXPOSE -> web_ports prefill (pull only; a build's EXPOSE is the base image's)
		JSON web_ports = df_mode ? JSON::Array() : emit::web_ports_from_image(out + "/image-config.json");
		std::string stop_sig = df_mode ? std::string() : emit::stop_signal_from_image(out + "/image-config.json");
			// --rw-overlay/--dev: persistent, resettable r/w overlay (base rootfs stays pristine)
			std::string overlay = ( o.rw_overlay || o.dev ) ? abs_out + ".overlay" : std::string();
			if ( !overlay.empty()) mkdir(overlay.c_str(), 0700);   // ujail -O needs the dir to exist
		if ( !reg::register_container(o.uxc_dir, name, abs_out, prov_image, prov_digest, o.infra, o.autostart, web_ports, stop_sig, overlay, err)) { err = "register: " + err; return false; }
		logger::info << "==> registered: " << o.uxc_dir << "/" << name << ".json" << std::endl;
		// a profile's "_registry" half: devices, shm_size, volumes, healthcheck,
		// notes... the things that cannot live in an OCI config but are exactly
		// what makes an application container actually run
		if ( pinfo.registry.type() == JSON::TYPE::OBJECT && pinfo.registry.begin() != pinfo.registry.end()) {
			std::vector<std::string> applied;
			std::string rerr;
			if ( !reg::apply_profile_registry(o.uxc_dir, name, pinfo.registry, applied, rerr))
				logger::error << "profile registry: " << rerr << std::endl;      // non-fatal: the bundle is fine
			else if ( !applied.empty()) {
				std::string s;
				for ( const std::string& k : applied ) s += ( s.empty() ? "" : ", " ) + k;
				logger::info << "    profile set: " << s << std::endl;
			}
		}
		// starting config files the application cannot come up without
		{
			std::vector<std::string> seeded = emit::seed_files(pinfo.seed);
			for ( const std::string& s : seeded )
				logger::info << "    wrote a starting config: " << s << " (edit it before going live)" << std::endl;
		}
		if ( !pinfo.needs.empty()) {
			logger::info << "    the profile needs these host paths to exist before a start:" << std::endl;
			for ( const std::string& p : pinfo.needs ) {
				struct stat nst;
				logger::info << "      " << p << ( stat(p.c_str(), &nst) == 0 ? "" : "   <-- MISSING" ) << std::endl;
			}
		}
		if ( web_ports.begin() != web_ports.end())
			logger::info << "    web UI port(s) detected from EXPOSE - review/label them in LuCI" << std::endl;
	}

	o.result_name = name;
	o.result_path = abs_out;
	logger::info << "==> done: " << name << std::endl;
	return true;
}

} // namespace docker2uxc
