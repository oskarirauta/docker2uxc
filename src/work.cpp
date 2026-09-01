#include "work.hpp"

#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unistd.h>
#include <ftw.h>
#include <sys/mount.h>

namespace work {

std::atomic<bool> cancelled{false};

static void on_signal(int) { cancelled = true; }   // async-signal-safe: set the flag only

void install_signal_handlers() {
	struct sigaction sa;
	std::memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT,  &sa, nullptr);
	std::signal(SIGPIPE, SIG_IGN);
}

static int rm_entry(const char* p, const struct stat*, int, struct FTW*) {
	remove(p);   // file or now-empty dir (FTW_DEPTH gives children first)
	return 0;
}

Dir::Dir(const std::string& base) {
	// The scratch dir holds a whole DECOMPRESSED layer at a time (gigabytes for
	// an image like Frigate). On OpenWrt /tmp is a RAM tmpfs, so the caller can
	// hand us the bundle's filesystem instead and keep that out of memory.
	std::string b = base;
	if ( b.empty()) { const char* e = getenv("TMPDIR"); if ( e && *e ) b = e; }
	if ( b.empty()) b = "/tmp";
	while ( b.size() > 1 && b.back() == '/' ) b.pop_back();

	std::string tmpl = b + "/docker2uxc-work-XXXXXX";
	std::vector<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');
	char* d = mkdtemp(buf.data());
	if ( d ) { path_ = d; return; }
	if ( b == "/tmp" ) return;
	char fallback[] = "/tmp/docker2uxc-work-XXXXXX";     // e.g. a read-only bundle parent
	if (( d = mkdtemp(fallback))) path_ = d;
}

Dir::~Dir() { cleanup(); }

void Dir::cleanup() {
	if ( done_ || path_.empty()) return;
	done_ = true;
	// unmount registered binds (reverse order, lazy) BEFORE removing the tree
	for ( auto it = mounts_.rbegin(); it != mounts_.rend(); ++it )
		umount2(it->c_str(), MNT_DETACH);
	// FTW_MOUNT: never cross a mount point - if any bind survived (e.g. a SIGKILL
	// before the umount above), don't recurse into it (host /proc, /dev, /sys)
	nftw(path_.c_str(), rm_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

}
