#pragma once
#include <string>

namespace extract {

// Decompress + securely extract one downloaded layer file into `rootfs`,
// applying overlayfs whiteouts (.wh.* and .wh..wh..opq). `tmp` is a scratch
// path used for decompression. SECURE BY CONSTRUCTION: rejects "..", strips
// absolute paths, and never writes through a symlinked path component (it
// replaces an intervening symlink with a real directory) - so a malicious or
// typosquatted image cannot escape rootfs (the shell's tar-symlink RCE). Layers
// must be applied in order. Returns false + err.
bool layer(const std::string& layerfile, const std::string& rootfs, const std::string& tmp, std::string& err);

}
