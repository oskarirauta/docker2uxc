#pragma once
#include <string>
#include <vector>

namespace bundle {

struct Opts {
	std::string name;
	std::string caps = "permissive";   // "permissive" | "minimal"
	bool nnp = true;                    // process.noNewPrivileges (--privileged -> false)
	bool network_isolated = false;      // --network isolated -> own netns
	bool resolvconf = false;            // --resolv-conf -> bind /etc/resolv.conf
	bool accounting = true;             // linux.resources memory+pids (cgroup stats)
	std::vector<std::string> args_override;  // if set, replaces the image Entrypoint+Cmd (e.g. --dev idle init)
};

// Build an OCI runtime config.json from the image config blob + opts and write
// it to `out_config_json`. `rootfs_dir` is used to resolve a User name -> uid/gid.
bool write_config(const std::string& image_config_blob, const std::string& rootfs_dir,
                  const Opts& opts, const std::string& out_config_json, std::string& err);

}
