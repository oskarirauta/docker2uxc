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
#include <ftw.h>
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

static int rm_entry(const char* p, const struct stat*, int, struct FTW*) { remove(p); return 0; }
static void rm_rf(const std::string& path) { nftw(path.c_str(), rm_entry, 16, FTW_DEPTH | FTW_PHYS); }

// Ensure the parent dirs of `comps` exist as REAL dirs under rootfs, replacing
// any intervening symlink (or non-dir) with a fresh dir so we never write
// THROUGH a symlink out of rootfs. Sets `full` to the final entry path.
static bool secure_parents(const std::string& rootfs, const std::vector<std::string>& comps, std::string& full) {
	std::string cur = rootfs;
	for ( size_t i = 0; i + 1 < comps.size(); i++ ) {
		cur += "/" + comps[i];
		struct stat st;
		if ( lstat(cur.c_str(), &st) == 0 ) {
			if ( !S_ISDIR(st.st_mode)) {            // symlink or file where a dir must be
				if ( S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
					if ( remove(cur.c_str()) != 0 ) { rm_rf(cur); }
				}
				if ( mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST ) return false;
			}
		} else {
			if ( mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST ) return false;
		}
	}
	full = cur + "/" + comps.back();
	return true;
}

static bool write_file(const std::string& full, FILE* tar, unsigned long long size, mode_t mode) {
	unlink(full.c_str());   // drop any existing symlink/file (overlay: layer replaces)
	int fd = open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode & 0777);
	if ( fd < 0 ) return false;
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
	// skip padding to the 512-byte block boundary
	unsigned long long pad = (512 - (size % 512)) % 512;
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
		if ( len == 0 || i + len > rec.size()) break;
		std::string kv = rec.substr(sp + 1, ( i + len ) - (sp + 1));
		if ( !kv.empty() && kv.back() == '\n' ) kv.pop_back();
		size_t eq = kv.find('=');
		if ( eq != std::string::npos && kv.substr(0, eq) == key ) return kv.substr(eq + 1);
		i += len;
	}
	return "";
}

static std::string read_data(FILE* f, unsigned long long size) {
	std::string s;
	s.resize(size);
	if ( size && std::fread(&s[0], 1, size, f) != size ) s.clear();
	unsigned long long pad = (512 - (size % 512)) % 512;
	if ( pad ) std::fseek(f, (long)pad, SEEK_CUR);
	return s;
}

static bool extract_tar(const std::string& tarfile, const std::string& rootfs, std::string& err) {
	FILE* f = std::fopen(tarfile.c_str(), "rb");
	if ( !f ) { err = "open tar " + tarfile; return false; }

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

		// whiteouts (operate relative to the entry's directory in rootfs)
		std::string base = comps.back();
		if ( base.rfind(".wh.", 0) == 0 ) {
			std::string dir = rootfs;
			for ( size_t i = 0; i + 1 < comps.size(); i++ ) dir += "/" + comps[i];
			if ( base == ".wh..wh..opq" ) {
				// opaque: remove the dir's existing contents
				rm_rf(dir);   // remove + recreate (its own layer entry, if any, re-adds it)
				mkdir(dir.c_str(), 0755);
			} else {
				rm_rf(dir + "/" + base.substr(4));   // remove the whited-out target
			}
			if ( blocks ) std::fseek(f, (long)(blocks * 512), SEEK_CUR);
			continue;
		}

		std::string full;
		if ( !secure_parents(rootfs, comps, full)) { err = "cannot create path for " + name; ok = false; break; }

		switch ( type ) {
			case '5':                                  // directory
				mkdir(full.c_str(), (mode_t)(octal(hdr + 100, 8) & 0777) | 0700);
				break;
			case '2':                                  // symlink (target NOT followed at write time)
				unlink(full.c_str());
				if ( symlink(link.c_str(), full.c_str()) != 0 ) { /* tolerate */ }
				break;
			case '1': {                                // hardlink (to another already-extracted entry)
				std::vector<std::string> lc;
				if ( sanitize(link, lc)) {
					std::string lf; std::string lroot = rootfs;
					for ( size_t i = 0; i + 1 < lc.size(); i++ ) lroot += "/" + lc[i];
					lf = lroot + "/" + lc.back();
					unlink(full.c_str());
					if ( ::link(lf.c_str(), full.c_str()) != 0 ) { /* tolerate */ }
				}
				break;
			}
			case '0': case '\0': case '7':             // regular file
				if ( !write_file(full, f, size, (mode_t)octal(hdr + 100, 8))) { err = "write " + name; ok = false; }
				continue;                              // write_file consumed the data + padding
			default:                                   // char/block/fifo: skip data, don't create device nodes
				break;
		}
		if ( blocks ) std::fseek(f, (long)(blocks * 512), SEEK_CUR);
		if ( !ok ) break;
	}

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
