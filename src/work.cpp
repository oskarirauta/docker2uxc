#include "work.hpp"

#include <csignal>
#include <cstring>
#include <cstdio>
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

Dir::Dir() {
	char tmpl[] = "/tmp/docker2uxc-work-XXXXXX";
	char* d = mkdtemp(tmpl);
	if ( d ) path_ = d;
}

Dir::~Dir() { cleanup(); }

void Dir::cleanup() {
	if ( done_ || path_.empty()) return;
	done_ = true;
	// unmount registered binds (reverse order, lazy) BEFORE removing the tree
	for ( auto it = mounts_.rbegin(); it != mounts_.rend(); ++it )
		umount2(it->c_str(), MNT_DETACH);
	nftw(path_.c_str(), rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

}
