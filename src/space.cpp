#include "space.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <ctime>
#include <cstdio>

namespace space {
namespace {

// linux/magic.h values; kept local so the header needs no kernel includes
constexpr unsigned long TMPFS_MAGIC = 0x01021994UL;
constexpr unsigned long RAMFS_MAGIC = 0x858458f6UL;

// the longest existing prefix of `path` ("/srv/uxc/frigate" -> "/srv" when only
// /srv exists yet), so a not-yet-created bundle dir still measures its target fs
std::string existing_ancestor(const std::string& path) {
	std::string p = path;
	if ( p.empty()) return ".";
	for ( ;; ) {
		struct stat st;
		if ( stat(p.c_str(), &st) == 0 ) return p;
		std::string::size_type s = p.find_last_of('/');
		if ( s == std::string::npos ) return ".";
		if ( s == 0 ) return "/";
		p = p.substr(0, s);
	}
}

struct Guard {
	bool armed = false;
	std::string path;
	unsigned long long floor = 0;
	long long last_ms = 0;
	unsigned long long since = 0;      // bytes written since the last statvfs
	bool tripped = false;
	std::string reason;
};

long long now_ms() {
	struct timespec ts;
	if ( clock_gettime(CLOCK_MONOTONIC, &ts) != 0 ) return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
Guard g;

} // anonymous namespace

Info of(const std::string& path) {
	Info i;
	std::string p = existing_ancestor(path);
	struct statvfs vfs;
	if ( statvfs(p.c_str(), &vfs) != 0 ) return i;
	i.ok    = true;
	i.mount = p;
	i.total = (unsigned long long)vfs.f_blocks * (unsigned long long)vfs.f_frsize;
	i.avail = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
	struct stat st;
	if ( stat(p.c_str(), &st) == 0 ) i.dev = (unsigned long long)st.st_dev;
	struct statfs sf;
	if ( statfs(p.c_str(), &sf) == 0 )
		i.tmpfs = (( unsigned long )sf.f_type == TMPFS_MAGIC || ( unsigned long )sf.f_type == RAMFS_MAGIC );
	return i;
}

bool same(const Info& a, const Info& b) { return a.ok && b.ok && a.dev == b.dev; }

std::string human(unsigned long long b) {
	static const char* U[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	double v = (double)b;
	int u = 0;
	while ( v >= 1024.0 && u < 4 ) { v /= 1024.0; u++; }
	char buf[48];
	snprintf(buf, sizeof(buf), ( u == 0 || v >= 100.0 ) ? "%.0f %s" : "%.1f %s", v, U[u]);
	return buf;
}

// 1% of the filesystem, clamped to [4 MiB, 32 MiB]. The cap keeps the reserve
// from swallowing a large disk; the lower bound keeps a small dedicated
// partition usable at all (a flat 32 MiB would make a 64 MiB /srv look full).
unsigned long long reserve(const Info& i) {
	unsigned long long r = i.total / 100ULL;
	if ( r <  4ULL * 1024 * 1024 ) r =  4ULL * 1024 * 1024;
	if ( r > 32ULL * 1024 * 1024 ) r = 32ULL * 1024 * 1024;
	return r;
}

unsigned long long usable(const Info& i) {
	unsigned long long r = reserve(i);
	return i.avail > r ? ( i.avail - r ) : 0ULL;
}

void arm(const std::string& path) {
	Info i = of(path);
	g = Guard();
	if ( !i.ok ) return;
	g.armed = true;
	g.path  = i.mount;
	// The watchdog floor is at least 16 MiB even when the preflight reserve is
	// smaller: this is the emergency brake, and between two polls a fast writer
	// gets through a lot of megabytes.
	g.floor = reserve(i);
	if ( g.floor < 16ULL * 1024 * 1024 ) g.floor = 16ULL * 1024 * 1024;
	g.last_ms = 0;
}

void disarm() { g = Guard(); }

bool low(unsigned long long wrote) {
	if ( !g.armed ) return false;
	if ( g.tripped ) return true;
	g.since += wrote;
	long long now = now_ms();
	if ( g.since < 8ULL * 1024 * 1024 && now && g.last_ms && now - g.last_ms < 250 ) return false;
	g.last_ms = now;
	g.since   = 0;
	struct statvfs vfs;
	if ( statvfs(g.path.c_str(), &vfs) != 0 ) return false;
	unsigned long long avail = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
	if ( avail >= g.floor ) return false;
	g.tripped = true;
	g.reason  = "out of space on " + g.path + ": only " + human(avail) +
	            " left (stopping at the " + human(g.floor) + " reserve so the system stays usable)";
	return true;
}

const std::string& why() { return g.reason; }

}
