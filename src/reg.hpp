#pragma once
#include <string>

namespace reg {

// Register (or re-register) the container in <uxc_dir>/<name>.json. Merges an
// overlay {name, path, image?, digest?, infra?, autostart?} onto any existing
// entry (overlay wins per key; existing volumes/env/devices/... survive a
// re-pull), exactly like the shell's `jq -s '.[0] + .[1]'`. Returns false + err.
bool register_container(const std::string& uxc_dir, const std::string& name, const std::string& abs_out,
                        const std::string& image, const std::string& digest, const std::string& infra,
                        bool autostart, std::string& err);

}
