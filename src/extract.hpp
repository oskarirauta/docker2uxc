#pragma once
#include <string>

namespace extract {

// Decompress + securely extract one downloaded layer file into `rootfs`,
// applying overlayfs whiteouts (.wh.* and .wh..wh..opq). `tmp` is a scratch
// path used for decompression. SECURE BY CONSTRUCTION against MALICIOUS LAYER
// CONTENT: rejects "..", strips absolute paths, and never writes through a
// symlinked path component (it replaces an intervening symlink with a real dir
// right before the O_NOFOLLOW open) - so a malicious or typosquatted image
// cannot escape rootfs (the shell's tar-symlink RCE). This holds because
// extraction is single-threaded and we are the only writer to `rootfs`; it does
// NOT defend against a separate process concurrently racing symlinks into
// `rootfs` mid-extraction (out of scope: rootfs is a fresh root-owned dir we
// create). Layers must be applied in order. Returns false + err.
bool layer(const std::string& layerfile, const std::string& rootfs, const std::string& tmp, std::string& err);

}
