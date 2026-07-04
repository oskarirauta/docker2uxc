#include "extract.hpp"
#include "work.hpp"

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <zlib.h>
#include <zstd.h>
#include <lzma.h>

namespace extract {

// ----------------------------------------------------------- decompression ----

enum Fmt { GZIP, ZSTD, XZ, PLAIN };

static Fmt detect(const std::string& file) {
	FILE* f = std::fopen(file.c_str(), "rb");
	if ( !f ) return PLAIN;
	unsigned char m[6] = {0};
	size_t n = std::fread(m, 1, 6, f);
	std::fclose(f);
	if ( n >= 2 && m[0] == 0x1f && m[1] == 0x8b ) return GZIP;
	if ( n >= 4 && m[0] == 0x28 && m[1] == 0xb5 && m[2] == 0x2f && m[3] == 0xfd ) return ZSTD;
	if ( n >= 6 && m[0] == 0xfd && m[1] == '7' && m[2] == 'z' && m[3] == 'X' && m[4] == 'Z' && m[5] == 0x00 ) return XZ;
	return PLAIN;
}

static bool decomp_gzip(const std::string& in, const std::string& out, std::string& err) {
	gzFile gz = gzopen(in.c_str(), "rb");
	if ( !gz ) { err = "gzopen failed"; return false; }
	FILE* of = std::fopen(out.c_str(), "wb");
	if ( !of ) { gzclose(gz); err = "cannot write " + out; return false; }
	char buf[65536];
	int n; bool ok = true;
	while (( n = gzread(gz, buf, sizeof buf)) > 0 ) {
		if ( work::cancelled ) { err = "cancelled"; ok = false; break; }
		if ( std::fwrite(buf, 1, n, of) != (size_t)n ) { err = "write error"; ok = false; break; }
	}
	if ( n < 0 ) { err = "gzip decode error"; ok = false; }
	gzclose(gz); std::fclose(of);
	return ok;
}

static bool decomp_zstd(const std::string& in, const std::string& out, std::string& err) {
	FILE* inf = std::fopen(in.c_str(), "rb");
	if ( !inf ) { err = "open " + in; return false; }
	FILE* of = std::fopen(out.c_str(), "wb");
	if ( !of ) { std::fclose(inf); err = "cannot write " + out; return false; }
	ZSTD_DStream* ds = ZSTD_createDStream();
	ZSTD_initDStream(ds);
	size_t inCap = ZSTD_DStreamInSize(), outCap = ZSTD_DStreamOutSize();
	std::vector<char> ibuf(inCap), obuf(outCap);
	bool ok = true; size_t n;
	while (( n = std::fread(ibuf.data(), 1, inCap, inf)) > 0 ) {
		ZSTD_inBuffer zin{ ibuf.data(), n, 0 };
		while ( zin.pos < zin.size ) {
			if ( work::cancelled ) { err = "cancelled"; ok = false; break; }
			ZSTD_outBuffer zout{ obuf.data(), outCap, 0 };
			size_t r = ZSTD_decompressStream(ds, &zout, &zin);
			if ( ZSTD_isError(r)) { err = std::string("zstd: ") + ZSTD_getErrorName(r); ok = false; break; }
			if ( std::fwrite(obuf.data(), 1, zout.pos, of) != zout.pos ) { err = "write error"; ok = false; break; }
		}
		if ( !ok ) break;
	}
	if ( ok && std::ferror(inf)) { err = "read error on " + in; ok = false; }   // fread()==0 can be error, not just EOF
	ZSTD_freeDStream(ds); std::fclose(inf); std::fclose(of);
	return ok;
}

static bool decomp_xz(const std::string& in, const std::string& out, std::string& err) {
	FILE* inf = std::fopen(in.c_str(), "rb");
	if ( !inf ) { err = "open " + in; return false; }
	FILE* of = std::fopen(out.c_str(), "wb");
	if ( !of ) { std::fclose(inf); err = "cannot write " + out; return false; }
	lzma_stream strm = LZMA_STREAM_INIT;
	if ( lzma_stream_decoder(&strm, UINT64_MAX, 0) != LZMA_OK ) {
		std::fclose(inf); std::fclose(of); err = "lzma init failed"; return false;
	}
	uint8_t ibuf[65536], obuf[65536];
	lzma_action action = LZMA_RUN;
	strm.next_in = nullptr; strm.avail_in = 0;
	bool ok = true;
	for (;;) {
		if ( strm.avail_in == 0 && !std::feof(inf)) {
			strm.next_in = ibuf;
			strm.avail_in = std::fread(ibuf, 1, sizeof ibuf, inf);
			if ( std::ferror(inf)) { err = "read error on " + in; ok = false; break; }   // else a read error spins forever (avail_in stays 0, !feof)
			if ( std::feof(inf)) action = LZMA_FINISH;
		}
		strm.next_out = obuf; strm.avail_out = sizeof obuf;
		lzma_ret r = lzma_code(&strm, action);
		size_t have = sizeof obuf - strm.avail_out;
		if ( have && std::fwrite(obuf, 1, have, of) != have ) { err = "write error"; ok = false; break; }
		if ( r == LZMA_STREAM_END ) break;
		if ( r != LZMA_OK ) { err = "xz decode error"; ok = false; break; }
		if ( work::cancelled ) { err = "cancelled"; ok = false; break; }
	}
	lzma_end(&strm); std::fclose(inf); std::fclose(of);
	return ok;
}

// ------------------------------------------------------------- tar + secure ---

static unsigned long long octal(const char* p, size_t len) {
	unsigned long long v = 0;
	for ( size_t i = 0; i < len; i++ ) {
		char c = p[i];
		if ( c < '0' || c > '7' ) continue;   // skip leading spaces / trailing NUL+space
		v = v * 8 + (unsigned)(c - '0');
	}
	return v;
}

static std::string field(const char* p, size_t max) {
	size_t n = 0;
	while ( n < max && p[n] ) n++;
	return std::string(p, n);
}

// normalize a tar path under rootfs: drop leading '/', reject any ".." or "."
// component and empties. Returns false to reject the entry.
static bool sanitize(const std::string& name, std::vector<std::string>& comps) {
	comps.clear();
	std::string cur;
	for ( size_t i = 0; i <= name.size(); i++ ) {
		char c = ( i < name.size()) ? name[i] : '/';
		if ( c == '/' ) {
			if ( cur.empty() || cur == "." ) { cur.clear(); continue; }
			if ( cur == ".." ) return false;        // traversal: reject the whole entry
			comps.push_back(cur); cur.clear();
		} else cur += c;
	}
	return !comps.empty();
}

// All path resolution below is by directory fd with O_NOFOLLOW on EVERY
// component (parents included), so no symlink is ever traversed - this closes
// the TOCTOU window that a string path + final-only O_NOFOLLOW would leave open.

// Open (creating as needed) the parent dir of `comps` under root_fd. Each step
// is O_NOFOLLOW; a symlink/non-dir in the way is replaced by a real dir. Returns
// the parent fd (caller closes) or -1; sets `base` to the final component.
static int open_parent(int root_fd, const std::vector<std::string>& comps, std::string& base) {
	int dir = openat(root_fd, ".", O_RDONLY | O_DIRECTORY);   // a fresh fd at rootfs
	if ( dir < 0 ) return -1;
	for ( size_t i = 0; i + 1 < comps.size(); i++ ) {
		const char* c = comps[i].c_str();
		int next = openat(dir, c, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		if ( next < 0 ) {                              // missing, or a symlink/file in the way
			unlinkat(dir, c, 0);                       // drop a symlink/file (ENOENT ignored)
			if ( mkdirat(dir, c, 0755) != 0 && errno != EEXIST ) { close(dir); return -1; }
			next = openat(dir, c, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			if ( next < 0 ) { close(dir); return -1; }
		}
		close(dir);
		dir = next;
	}
	base = comps.back();
	return dir;
}

// fd-relative recursive remove (whiteouts) - never follows a symlink
static void rm_at(int pfd, const char* name) {
	if ( unlinkat(pfd, name, 0) == 0 ) return;         // file/symlink
	int dfd = openat(pfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if ( dfd < 0 ) { unlinkat(pfd, name, 0); return; }
	if ( DIR* d = fdopendir(dfd)) {
		for ( struct dirent* e; ( e = readdir(d)); ) {
			if ( !std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
			rm_at(dfd, e->d_name);
		}
		closedir(d);                                   // also closes dfd
	} else close(dfd);
	unlinkat(pfd, name, AT_REMOVEDIR);
}

// empty a directory given its fd (opaque whiteout)
static void clear_dir(int dfd) {
	int c = dup(dfd);
	if ( c < 0 ) return;
	DIR* d = fdopendir(c);
	if ( !d ) { close(c); return; }
	for ( struct dirent* e; ( e = readdir(d)); ) {
		if ( !std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
		rm_at(dfd, e->d_name);
	}
	closedir(d);
}

static bool write_file_at(int pfd, const std::string& base, FILE* tar, unsigned long long size, mode_t mode) {
	unlinkat(pfd, base.c_str(), 0);   // drop any existing symlink/file (overlay: layer replaces)
	int fd = openat(pfd, base.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode & 0777);
	if ( fd < 0 ) {
		// consume the entry's data + padding anyway, otherwise the caller's
		// `continue` leaves the tar stream misaligned (payload read as a header)
		unsigned long long skip = size + (512 - (size % 512)) % 512;
		std::fseek(tar, (long)skip, SEEK_CUR);
		return false;
	}
	unsigned long long left = size;
	char buf[65536];
	bool ok = true;
	while ( left > 0 ) {
		size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
		size_t n = std::fread(buf, 1, want, tar);
		if ( n == 0 ) { ok = false; break; }
		if ( write(fd, buf, n) != (ssize_t)n ) { ok = false; break; }
		left -= n;
	}
	close(fd);
	unsigned long long pad = (512 - (size % 512)) % 512;   // skip to the 512-byte block boundary
	if ( pad ) std::fseek(tar, (long)pad, SEEK_CUR);
	return ok;
}

// PAX record scan: "<len> key=value\n" repeated. Return value of `key`, or "".
static std::string pax_get(const std::string& rec, const std::string& key) {
	size_t i = 0;
	while ( i < rec.size()) {
		size_t sp = rec.find(' ', i);
		if ( sp == std::string::npos ) break;
		unsigned long len = std::strtoul(rec.c_str() + i, nullptr, 10);
		if ( len == 0 || i + len > rec.size() || sp + 1 > i + len ) break;   // malformed: space past record end would underflow the substr length
		std::string kv = rec.substr(sp + 1, ( i + len ) - (sp + 1));
		if ( !kv.empty() && kv.back() == '\n' ) kv.pop_back();
		size_t eq = kv.find('=');
		if ( eq != std::string::npos && kv.substr(0, eq) == key ) return kv.substr(eq + 1);
		i += len;
	}
	return "";
}

static std::string read_data(FILE* f, unsigned long long size) {
	unsigned long long pad = (512 - (size % 512)) % 512;
	// GNU long-name/link and PAX records are a few KB; a huge size is a crafted or
	// corrupt layer. Never resize() to an attacker-controlled length (OOM / throw) -
	// discard the record in bounded chunks so the tar stream stays aligned.
	if ( size > (1ULL << 20) ) {
		char skip[4096];
		unsigned long long left = size + pad;
		while ( left ) {
			size_t chunk = left < sizeof(skip) ? (size_t)left : sizeof(skip);
			size_t got = std::fread(skip, 1, chunk, f);
			if ( !got ) break;   // EOF/error: extraction ends upstream
			left -= got;
		}
		return "";
	}
	std::string s;
	s.resize(size);
	if ( size && std::fread(&s[0], 1, size, f) != size ) s.clear();
	if ( pad ) std::fseek(f, (long)pad, SEEK_CUR);
	return s;
}

static bool extract_tar(const std::string& tarfile, const std::string& rootfs, std::string& err) {
	FILE* f = std::fopen(tarfile.c_str(), "rb");
	if ( !f ) { err = "open tar " + tarfile; return false; }
	int root_fd = open(rootfs.c_str(), O_RDONLY | O_DIRECTORY);   // base for all O_NOFOLLOW openat() resolution
	if ( root_fd < 0 ) { std::fclose(f); err = "open rootfs " + rootfs; return false; }

	std::string pend_name, pend_link;   // from GNU L/K or PAX path/linkpath
	char hdr[512];
	bool ok = true;

	while ( std::fread(hdr, 1, 512, f) == 512 ) {
		if ( work::cancelled ) { err = "cancelled"; ok = false; break; }

		bool zero = true;
		for ( int i = 0; i < 512; i++ ) if ( hdr[i] ) { zero = false; break; }
		if ( zero ) break;   // end of archive

		char type = hdr[156];
		unsigned long long size = octal(hdr + 124, 12);

		if ( type == 'L' ) { pend_name = read_data(f, size); if ( !pend_name.empty() && pend_name.back() == '\0' ) pend_name.pop_back(); continue; }
		if ( type == 'K' ) { pend_link = read_data(f, size); if ( !pend_link.empty() && pend_link.back() == '\0' ) pend_link.pop_back(); continue; }
		if ( type == 'x' || type == 'g' ) {
			std::string rec = read_data(f, size);
			std::string p = pax_get(rec, "path");     if ( !p.empty()) pend_name = p;
			std::string l = pax_get(rec, "linkpath");  if ( !l.empty()) pend_link = l;
			continue;
		}

		std::string name = !pend_name.empty() ? pend_name
		                   : ( field(hdr + 345, 155).empty() ? field(hdr, 100)
		                       : field(hdr + 345, 155) + "/" + field(hdr, 100));
		std::string link = !pend_link.empty() ? pend_link : field(hdr + 157, 100);
		pend_name.clear(); pend_link.clear();

		unsigned long long blocks = (size + 511) / 512;

		std::vector<std::string> comps;
		if ( !sanitize(name, comps)) { if ( blocks ) std::fseek(f, (long)(blocks * 512), SEEK_CUR); continue; }

		// whiteouts (operate relative to the entry's directory, by fd)
		std::string base = comps.back();
		if ( base.rfind(".wh.", 0) == 0 ) {
			std::string wb;
			int pfd = open_parent(root_fd, comps, wb);   // pfd = the entry's directory
			if ( pfd >= 0 ) {
				if ( base == ".wh..wh..opq" ) clear_dir(pfd);          // opaque: empty the dir
				else rm_at(pfd, base.substr(4).c_str());               // remove the whited-out target
				close(pfd);
			}
			if ( blocks ) std::fseek(f, (long)(blocks * 512), SEEK_CUR);
			continue;
		}

		std::string bn;
		int pfd = open_parent(root_fd, comps, bn);
		if ( pfd < 0 ) { err = "cannot create path for " + name; ok = false; break; }

		switch ( type ) {
			case '5':                                  // directory
				mkdirat(pfd, bn.c_str(), (mode_t)(octal(hdr + 100, 8) & 0777) | 0700);
				break;
			case '2':                                  // symlink (target stored as-is, never followed by us)
				unlinkat(pfd, bn.c_str(), 0);
				if ( symlinkat(link.c_str(), pfd, bn.c_str()) != 0 ) { /* tolerate */ }
				break;
			case '1': {                                // hardlink to another already-extracted entry
				std::vector<std::string> lc;
				if ( sanitize(link, lc)) {
					std::string lbn;
					int lpfd = open_parent(root_fd, lc, lbn);
					if ( lpfd >= 0 ) {
						unlinkat(pfd, bn.c_str(), 0);
						if ( linkat(lpfd, lbn.c_str(), pfd, bn.c_str(), 0) != 0 ) { /* tolerate */ }
						close(lpfd);
					}
				}
				break;
			}
			case '0': case '\0': case '7':             // regular file
				if ( !write_file_at(pfd, bn, f, size, (mode_t)octal(hdr + 100, 8))) { err = "write " + name; ok = false; }
				close(pfd);
				continue;                              // write_file_at consumed the data + padding
			default:                                   // char/block/fifo: skip data, don't create device nodes
				break;
		}
		close(pfd);
		if ( blocks ) std::fseek(f, (long)(blocks * 512), SEEK_CUR);
		if ( !ok ) break;
	}

	close(root_fd);
	std::fclose(f);
	return ok;
}

// ------------------------------------------------------------------- driver ---

bool layer(const std::string& layerfile, const std::string& rootfs, const std::string& tmp, std::string& err) {
	Fmt fmt = detect(layerfile);
	std::string tarpath = layerfile;
	if ( fmt != PLAIN ) {
		bool ok = ( fmt == GZIP ) ? decomp_gzip(layerfile, tmp, err)
		        : ( fmt == ZSTD ) ? decomp_zstd(layerfile, tmp, err)
		        :                   decomp_xz(layerfile, tmp, err);
		if ( !ok ) return false;
		tarpath = tmp;
	}
	bool ok = extract_tar(tarpath, rootfs, err);
	if ( fmt != PLAIN ) unlink(tmp.c_str());
	return ok;
}

}
