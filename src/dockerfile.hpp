#pragma once
#include <string>
#include <vector>

// Multi-stage Dockerfile build. Each FROM starts a stage; the last stage becomes
// the output bundle. A stage's base is either a pulled image or an earlier stage
// (FROM <name>), and COPY --from=<name|index> copies from an earlier stage's
// rootfs. "Support enough, not everything": ADD behaves like COPY (no URL fetch /
// tar auto-extract), no build args, no cache, no external-image COPY --from.
namespace dockerfile {

// One build stage: its FROM plus the instructions up to the next FROM (or EOF).
struct Stage {
	std::string name;                    // the `AS <name>`, or "" if unnamed
	std::string base_ref;                // the FROM argument (image ref or stage name)
	int         base_stage = -1;         // >=0 if base_ref names an earlier stage
	std::vector<std::string> lines;      // this stage's logical instruction lines
};

// A built stage, offered as a COPY --from= source (name/index -> its rootfs dir).
struct BuiltStage {
	std::string name;                    // the stage's AS name ("" if unnamed)
	int         index = -1;              // its position in the Dockerfile (0-based)
	std::string rootfs_dir;              // where its rootfs currently lives
};

// Parse a Dockerfile into its ordered stages (>=1). Returns false+err on: no FROM,
// an instruction before the first FROM, or a malformed FROM.
bool parse_stages(const std::string& dockerfile_path, std::vector<Stage>& out, std::string& err);

// last_use[j] = the highest stage index that consumes stage j (via COPY --from=j
// or a FROM basing on j), or -1 if nothing consumes it. Lets the builder free an
// intermediate stage's rootfs as soon as its last consumer is done.
std::vector<int> stage_last_use(const std::vector<Stage>& stages);

// Apply one stage's instructions on top of its already-prepared rootfs:
//   RUN        - run in the rootfs via chroot (/proc,/dev,/sys bound, the host
//                /etc/resolv.conf copied in for network), with the accumulated
//                ENV exports and a cd into the current WORKDIR
//   COPY/ADD   - copy <src>... into the rootfs (cp -a). --from=<name|index> takes
//                the source from an earlier stage's rootfs (via `built`) instead
//                of the build context.
//   ENV/WORKDIR/USER/CMD/ENTRYPOINT - mutate the OCI image config in place
//                (config_path is rewritten so bundle::write_config sees them)
// Requires root (chroot + bind mounts). Cancellable via work::cancelled. The bind
// mounts are ALWAYS torn down (MNT_DETACH) before this returns, so a later rm of
// the rootfs can never recurse into the host's /proc. Returns false+err.
bool apply_stage(const Stage& stage, const std::string& context_dir,
                 const std::string& rootfs_dir, const std::string& config_path,
                 const std::vector<BuiltStage>& built, std::string& err);

}
