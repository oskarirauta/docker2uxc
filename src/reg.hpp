#pragma once
#include <string>
#include "json.hpp"

namespace reg {

// Register (or re-register) the container in <uxc_dir>/<name>.json. Merges an
// overlay {name, path, image?, digest?, infra?, autostart?} onto any existing
// entry (overlay wins per key; existing volumes/env/devices/... survive a
// re-pull), exactly like the shell's `jq -s '.[0] + .[1]'`. Returns false + err.
// web_ports (EXPOSE-derived, may be empty) is applied ONLY when the existing
// entry has none - so a manual edit or re-pull never clobbers the user's list.
bool register_container(const std::string& uxc_dir, const std::string& name, const std::string& abs_out,
                        const std::string& image, const std::string& digest, const std::string& infra,
                        bool autostart, const JSON& web_ports, const std::string& stop_signal, std::string& err);

}
