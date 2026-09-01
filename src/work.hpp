#pragma once
#include <string>
#include <vector>
#include <atomic>

// Working directory + cancellation. The daemon cancels a job by SIGTERM'ing the
// process group; we catch it, set `cancelled` (async-signal-safe), let the long
// operations (download via curl, extraction) bail, and clean up via RAII.
namespace work {

extern std::atomic<bool> cancelled;
void install_signal_handlers();      // SIGTERM/SIGINT -> set cancelled; ignore SIGPIPE

// A temp dir that cleans itself up: unmounts any registered binds FIRST (so the
// recursive remove never descends into a bind-mounted /proc|/dev|/sys = the
// host's), then removes the tree. Used for $WORK (and Dockerfile-build later).
class Dir {
public:
	// `base` = where the scratch dir is created. Empty means $TMPDIR, else /tmp.
	// It holds one fully DECOMPRESSED layer at a time, so on a box whose /tmp is
	// a RAM tmpfs the caller should point this at the bundle's storage instead.
	explicit Dir(const std::string& base = "");
	~Dir();
	Dir(const Dir&) = delete;
	Dir& operator=(const Dir&) = delete;

	bool ok() const { return !path_.empty(); }
	const std::string& path() const { return path_; }
	void add_mount(const std::string& m) { mounts_.push_back(m); }
	void cleanup();                  // idempotent; also called by the destructor

private:
	std::string path_;
	std::vector<std::string> mounts_;
	bool done_ = false;
};

}
