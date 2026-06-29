#pragma once
#include <string>

// Single-stage Dockerfile build. The base image (FROM) is pulled like a normal
// image; this module then applies the remaining instructions on top of the
// already-extracted rootfs. "Support enough, not everything": ADD behaves like
// COPY (no URL fetch / tar auto-extract), no build args / multi-stage / cache.
namespace dockerfile {

// Extract the base image ref from the first FROM instruction. Returns "" and
// sets err on: no FROM, or a multi-stage build (a second FROM, or `FROM x AS y`).
std::string parse_from(const std::string& dockerfile_path, std::string& err);

// Apply the Dockerfile on top of an already-extracted rootfs:
//   RUN        - run in the rootfs via chroot (/proc,/dev,/sys bound, the host
//                /etc/resolv.conf copied in for network), with the accumulated
//                ENV exports and a cd into the current WORKDIR
//   COPY/ADD   - copy <src>... from the build context into the rootfs (cp -a)
//   ENV/WORKDIR/USER/CMD/ENTRYPOINT - mutate the OCI image config in place
//                (config_path is rewritten so bundle::write_config sees them)
// Requires root (chroot + bind mounts). Cancellable via work::cancelled - a
// SIGTERM mid-RUN kills the RUN process group and returns "cancelled". The
// bind mounts are ALWAYS torn down (MNT_DETACH) before this returns, so a later
// rm of the rootfs can never recurse into the host's /proc. Returns false+err.
bool apply(const std::string& dockerfile_path, const std::string& context_dir,
           const std::string& rootfs_dir, const std::string& config_path,
           std::string& err);

}
