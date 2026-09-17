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
//
// `build` is the BUILD provenance of a Dockerfile-built bundle:
//   { recipe?, dockerfile, context?, dockerfile_sha256, base, base_digest }
// It is the build-mode counterpart of image+digest: image/digest mean "this
// bundle is a pulled image" and drive a re-pull, `build` means "this bundle was
// built here" and drives a re-BUILD. A bundle has one or the other, never both -
// registering one therefore clears the other, so an upgrade can never re-pull a
// stock image over a locally built rootfs (which silently drops whatever the
// Dockerfile added: extensions, packages, patches).
bool register_container(const std::string& uxc_dir, const std::string& name, const std::string& abs_out,
                        const std::string& image, const std::string& digest, const std::string& infra,
                        bool autostart, const JSON& web_ports, const std::string& stop_signal,
                        const std::string& write_overlay_path, const JSON& build, std::string& err);

// Seed the registry entry with a profile's "_registry" block (devices, shm_size,
// volumes, healthcheck, notes, ...). Only keys the entry does NOT already carry
// are written, so a re-pull or an upgrade never overwrites what the user tuned
// by hand or in LuCI. `applied` lists the keys actually seeded. Identity and
// provenance keys (name/path/image/digest/created/build) are never written from
// a shared profile.
bool apply_profile_registry(const std::string& uxc_dir, const std::string& name, const JSON& block,
                            std::vector<std::string>& applied, std::string& err);

// Record which recipe produced this entry (top-level "recipe"), so `uxc deploy`
// and the LuCI Recipes page can tell a recipe-managed container from a hand-made
// one, and a redeploy knows which recipe to re-run. A build also carries it
// inside its `build` block; this is the pull-mode half. Never touches anything
// else in the entry.
bool record_recipe(const std::string& uxc_dir, const std::string& name, const std::string& recipe, std::string& err);

}
