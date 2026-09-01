#pragma once
#include <string>
#include <vector>
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
                        bool autostart, const JSON& web_ports, const std::string& stop_signal,
                        const std::string& write_overlay_path, std::string& err);

// Seed the registry entry with a profile's "_registry" block (devices, shm_size,
// volumes, healthcheck, notes, ...). Only keys the entry does NOT already carry
// are written, so a re-pull or an upgrade never overwrites what the user tuned
// by hand or in LuCI. `applied` lists the keys actually seeded.
bool apply_profile_registry(const std::string& uxc_dir, const std::string& name, const JSON& block,
                            std::vector<std::string>& applied, std::string& err);

}
