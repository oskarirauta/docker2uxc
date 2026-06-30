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

// Deep-merge <profile_dir>/<name>.json onto the bundle config.json (config_path)
// in place: objects merged key-by-key (overlay wins, recursing), arrays
// concatenated, scalars replaced; keys starting with '_' are stripped (profile
// comments). Returns false + err (profile missing, parse/merge/write error).
bool profile(const std::string& config_path, const std::string& dir, const std::string& name, std::string& err);

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
