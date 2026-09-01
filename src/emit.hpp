#pragma once
#include <string>
#include "json.hpp"

// Bundle-side output generators: profile overlay + the optional /etc/config
// network snippet, the procd "keeper" service, the README.notes guide, and a
// missing-bind-source warning. None of these are applied to the running system -
// they are written into the bundle for the user to review and install.
namespace emit {

// Resolve the profile directory: $DOCKER2UXC_PROFILES, else <exe-dir>/profiles
// (dev tree), else /usr/share/docker2uxc/profiles.
std::string profile_dir();

// Everything a profile says that is NOT part of the OCI config.json: the human
// description, the uxcd registry fields it seeds, and the host paths its binds
// and volumes need to exist. Filled by profile() and profile_info().
struct ProfileInfo {
	std::string name;
	std::string description;              // "_description"
	JSON registry = JSON::Object();       // "_registry": seeded into <uxc_dir>/<name>.json
	JSON seed = JSON::Object();           // "_seed": { host path: contents } written only when absent
	std::vector<std::string> needs;       // host paths that must exist before a start
	std::vector<std::string> devices;     // devices the profile passes through
	std::vector<std::string> caps_add;    // capabilities added on top of --caps
	std::vector<std::string> matches;     // "_matches": image repo names this profile is for
};

// Which profile in `dir` is meant for `image_ref`, or "". Matching is on the
// image's repository path: a profile's "_matches" entry hits when it equals the
// repo's last component ("frigate" for ghcr.io/blakeblackshear/frigate:0.18) or
// the whole repo path. Registry host and tag are ignored, so the same profile
// covers docker.io, ghcr.io and a private mirror.
std::string match_profile(const std::string& dir, const std::string& image_ref);

// Read <dir>/<name>.json WITHOUT applying it - for `uxc profiles` and the LuCI
// dropdown, so a user can see what a profile does before choosing it.
bool profile_info(const std::string& dir, const std::string& name, ProfileInfo& out, std::string& err);

// Profile names in `dir`, sorted, "_"-prefixed templates skipped.
std::vector<std::string> profile_names(const std::string& dir);

// Deep-merge <profile_dir>/<name>.json onto the bundle config.json (config_path)
// in place. Merge rules:
//   - objects      merged key by key, recursing
//   - mounts[]     merged BY DESTINATION (the profile's entry replaces the base
//                  mount at the same path; two mounts on one destination make
//                  ujail reject the whole spec)
//   - other arrays concatenated
//   - capability sets REPLACED (a profile can scope caps down); "_caps_add"
//                  instead ADDS to whatever --caps produced - almost always
//                  what an application profile wants
//   - a mount marked "_optional": true is dropped when its host source is
//                  absent, so a profile can offer /dev/dri or /dev/bus/usb
//                  without breaking boxes that have neither
// Keys starting with '_' are stripped from the result (they are the profile's
// own directives and comments). Returns false + err (profile missing, parse,
// merge or write error). `info`, when given, receives the non-OCI half.
bool profile(const std::string& config_path, const std::string& dir, const std::string& name,
             ProfileInfo* info, std::string& err);

// Write a profile's "_seed" files ({ host path: contents }). A file that already
// exists is never touched, parent directories are created. This is what lets a
// profiled pull produce a container that actually starts: an application whose
// config file must exist before its first run (mosquitto.conf, ...) gets a
// commented starting point instead of a crash loop. Returns the paths written.
std::vector<std::string> seed_files(const JSON& seed);

// Write <out>/network.uci - an /etc/config/network veth/bridge/infra snippet for
// an isolated container. Never applied automatically.
bool netconfig(const std::string& out, const std::string& name, const std::string& bridge, std::string& err);

// Write <out>/<name>.init - a procd "keeper" service (Docker-style auto-restart
// that keeps the container uxc-managed). abs_bundle = absolute bundle path.
bool keeper(const std::string& out, const std::string& name, const std::string& abs_bundle, std::string& err);

struct NotesInfo {
	std::string out;                // bundle directory
	std::string name;
	std::string version;            // docker2uxc version string
	std::string source;            // "reg/repo:tag"
	std::string arch;
	std::string config_digest;
	std::string image_config_path;  // <out>/image-config.json (ExposedPorts/Volumes)
	std::string config_json_path;   // <out>/config.json (process.args)
	std::string abs_out;            // absolute bundle path
	std::string network;           // "host" | "isolated"
	bool rw_overlay = false;
	bool emit_keeper = false;
};
// Write <out>/README.notes - a human-readable install/network/ports/volumes guide.
bool notes(const NotesInfo& ni, std::string& err);

// Pre-fill web_ports from the image's EXPOSE list (image-config.json ->
// .config.ExposedPorts), keeping only web-typical tcp ports. Returns a JSON
// array [{ port, scheme? }] (empty if none). register_container applies it only
// when the entry has no web_ports yet, so manual edits/re-pulls are preserved.
JSON web_ports_from_image(const std::string& image_config_path);

// Pre-fill stop_signal from the image's STOPSIGNAL (.config.StopSignal); "" if none.
// register_container applies it only when the entry has no stop_signal yet.
std::string stop_signal_from_image(const std::string& image_config_path);

// Warn (to the log) about bind mount sources in config.json that don't exist on
// the host yet - ujail fails the whole container if any bind source is missing.
void warn_missing_binds(const std::string& config_json_path);

}
