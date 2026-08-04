#pragma once
#include <string>

// The converter as a library: a host process (the CLI, or uxcd after fork()) can
// drive a pull/build directly via convert(Options) instead of exec'ing the tool.
// convert() does chroot()/mount() for Dockerfile builds and runs long downloads,
// so a daemon MUST fork() first and call it in the child (never in-process).
namespace docker2uxc {

inline constexpr const char* VERSION = "0.2.0-dev";

struct Options {
	// what to convert (exactly one of image / dockerfile)
	std::string image;        // image ref to pull (pull mode)
	std::string dockerfile;   // path to a Dockerfile (build mode; FROM is the base)
	std::string context;      // build context for COPY/ADD (default: Dockerfile's dir)

	// output / identity
	std::string out;          // output bundle dir (default: ./<name>)
	std::string name;         // container name (default: repo basename, or context dir in build mode)
	std::string arch;         // target arch e.g. "arm64", "arm/v7" (default: host)

	// registry
	std::string auth_file = "/etc/uxcd/auth.json";
	std::string cache_dir = "/tmp/docker2uxc-cache";   // "" disables the blob cache
	bool verify = true;       // sha256-verify downloaded blobs
	bool force  = false;      // overwrite an existing output bundle

	// bundle knobs
	std::string caps = "permissive";   // permissive | minimal
	bool privileged       = false;     // process.noNewPrivileges = false
	bool network_isolated = false;     // own netns (else share host net)
	bool resolvconf       = false;     // bind-mount host /etc/resolv.conf
	bool accounting       = true;      // memory+pids linux.resources block
	bool rw_overlay       = false;
	bool dev              = false;     // dev container: idle cntrinit init + writable persistent overlay
	std::string cntrinit  = "/usr/bin/cntrinit";  // static init copied into the bundle for --dev
	std::string profile;               // profiles/<profile>.json overlay

	// optional emitters
	bool emit_netconfig = false;
	std::string net_bridge = "br-lan";
	bool emit_keeper = false;

	// registration into the uxcd registry
	bool do_register = true;
	std::string uxc_dir = "/etc/uxc";
	std::string infra;
	bool autostart = false;

	// filled on success
	std::string result_name;
	std::string result_path;   // absolute bundle path
};

// Pull (or build, if opts.dockerfile is set) into an OCI bundle and register it
// (unless do_register is false). Progress goes to logger::; on failure returns
// false and sets err. Honors work::cancelled. The CALLER owns http::global_init/
// cleanup() and (for cancellation) work::install_signal_handlers().
bool convert(Options& opts, std::string& err);

// The digest `image` resolves to ("sha256:..."), or "" + err.
std::string resolve_digest(const std::string& image, const std::string& auth_file, std::string& err);

// For each registered container in uxc_dir that carries image+digest, re-resolve
// the upstream digest and append "<name>\t<current|update|error>\t<digest>\n" to
// `out`. Returns false + err only on a hard failure (a missing registry is fine).
bool check_updates(const std::string& uxc_dir, const std::string& auth_file, std::string& out, std::string& err);

}
