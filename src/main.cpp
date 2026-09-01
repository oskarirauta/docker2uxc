#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>

#include "usage.hpp"
#include "logger.hpp"
#include "http.hpp"
#include "work.hpp"
#include "convert.hpp"
#include "emit.hpp"

// Thin CLI over the docker2uxc converter library: parse args into Options and
// hand off to docker2uxc::convert / resolve_digest / check_updates.

int main(int argc, char** argv) {

	usage_t usage = {
		.args = { argc, argv },
		.info = {
			.name = "docker2uxc",
			.version_title = "version ",
			.version = docker2uxc::VERSION,
			.copyright = "2026, Oskari Rauta",
			.usage = "[options] <image-ref>",
			.description = "\nOCI image -> ujail/uxc bundle converter (C++)",
		},
		.options = {
			{ "out",            { .key = "o", .word = "out",            .desc = "output bundle dir (default ./<name>: HERE)", .flag = usage_t::REQUIRED, .name = "dir" }},
			{ "name",           { .key = "n", .word = "name",           .desc = "container name",                            .flag = usage_t::REQUIRED, .name = "name" }},
			{ "arch",           { .key = "a", .word = "arch",           .desc = "target architecture (default: host)",      .flag = usage_t::REQUIRED, .name = "arch" }},
			{ "dockerfile",     { .key = "d", .word = "dockerfile",     .desc = "build from a Dockerfile (FROM = base image)", .flag = usage_t::REQUIRED, .name = "file" }},
			{ "context",        {             .word = "context",        .desc = "build context for COPY/ADD (default: Dockerfile dir)", .flag = usage_t::REQUIRED, .name = "dir" }},
			{ "auth-file",      {             .word = "auth-file",      .desc = "registry credentials (Docker auths JSON)", .flag = usage_t::REQUIRED, .name = "file" }},
			{ "infra",          {             .word = "infra",          .desc = "register as a member of shared netns NAME", .flag = usage_t::REQUIRED, .name = "netns" }},
			{ "caps",           {             .word = "caps",           .desc = "capability set: permissive | minimal",      .flag = usage_t::REQUIRED, .name = "set" }},
			{ "profile",        {             .word = "profile",        .desc = "apply profiles/<NAME>.json overlay",        .flag = usage_t::REQUIRED, .name = "name" }},
			{ "profiles",       {             .word = "profiles",       .desc = "list the available profiles, then exit" }},
			{ "network",        {             .word = "network",        .desc = "host | isolated (default host)",            .flag = usage_t::REQUIRED, .name = "mode" }},
			{ "emit-netconfig", {             .word = "emit-netconfig", .desc = "write an /etc/config/network veth/infra snippet" }},
			{ "net-bridge",     {             .word = "net-bridge",     .desc = "bridge for --emit-netconfig (default br-lan)", .flag = usage_t::REQUIRED, .name = "br" }},
			{ "emit-keeper",    {             .word = "emit-keeper",    .desc = "write <name>.init: a procd keeper service" }},
			{ "privileged",     {             .word = "privileged",     .desc = "process.noNewPrivileges = false" }},
			{ "resolv-conf",    {             .word = "resolv-conf",    .desc = "bind-mount the host /etc/resolv.conf" }},
			{ "no-accounting",  {             .word = "no-accounting",  .desc = "omit the memory+pids linux.resources block" }},
			{ "no-verify",      {             .word = "no-verify",      .desc = "skip sha256 verification of downloaded blobs" }},
			{ "cache",          {             .word = "cache",          .desc = "blob cache directory (default /tmp/docker2uxc-cache)", .flag = usage_t::REQUIRED, .name = "dir" }},
			{ "rw-overlay",     {             .word = "rw-overlay",     .desc = "writable rootfs via a persistent overlay (base image stays pristine)" }},
			{ "dev",            {             .word = "dev",            .desc = "dev container: idle cntrinit init + writable overlay (shell in with uxe)" }},
			{ "cntrinit",       {             .word = "cntrinit",       .desc = "cntrinit binary to stage for --dev (default /usr/bin/cntrinit)", .flag = usage_t::REQUIRED, .name = "path" }},
			{ "autostart",      {             .word = "autostart",      .desc = "register the container to start on boot" }},
			{ "no-register",    {             .word = "no-register",    .desc = "build the bundle only; do not register with uxcd" }},
			{ "resolve-digest", {             .word = "resolve-digest", .desc = "print the digest the ref resolves to, then exit" }},
			{ "check-updates",  {             .word = "check-updates",  .desc = "report which registered containers have updates" }},
			{ "force",          { .key = "f", .word = "force",          .desc = "overwrite an existing output bundle" }},
			{ "verbose",        { .key = "v", .word = "verbose",        .desc = "verbose / debug logging" }},
			{ "help",           { .key = "h", .word = "help",           .desc = "show this help" }},
			{ "version",        { .key = "V", .word = "version",        .desc = "show version" }},
		}
	};

	if ( (bool)usage["version"] ) { std::cout << usage.version() << std::endl; return 0; }
	if ( (bool)usage["help"] )    { std::cout << usage << "\n" << usage.help() << std::endl; return 0; }
	if ( (bool)usage["verbose"] ) logger::loglevel(logger::debug);

	std::string auth_file = (bool)usage["auth-file"] ? usage["auth-file"].value : "/etc/uxcd/auth.json";
	http::global_init();
	work::install_signal_handlers();

	auto fin = [&](int rc) { http::global_cleanup(); return rc; };

	// --profiles: what overlays this install has, and what each one does.
	if ( (bool)usage["profiles"] ) {
		std::string dir = emit::profile_dir();
		std::vector<std::string> names = emit::profile_names(dir);
		if ( names.empty()) std::cout << "no profiles in " << dir << std::endl;
		else std::cout << "profiles in " << dir << ":" << std::endl;
		for ( const std::string& n : names ) {
			emit::ProfileInfo pi;
			std::string perr;
			if ( !emit::profile_info(dir, n, pi, perr)) { std::cout << "  " << n << "\t(" << perr << ")" << std::endl; continue; }
			std::cout << "  " << n << ( pi.description.empty() ? "" : "  -  " + pi.description ) << std::endl;
			if ( !pi.caps_add.empty()) {
				std::cout << "      adds capabilities:";
				for ( const std::string& c : pi.caps_add ) std::cout << " " << c;
				std::cout << std::endl;
			}
			if ( !pi.devices.empty()) {
				std::cout << "      devices:";
				for ( const std::string& d : pi.devices ) std::cout << " " << d;
				std::cout << std::endl;
			}
			for ( const std::string& p : pi.needs ) {
				struct stat nst;
				std::cout << "      needs host path: " << p << ( stat(p.c_str(), &nst) == 0 ? "" : "   (missing)" ) << std::endl;
			}
		}
		return fin(0);
	}

	// --check-updates: no positional ref; loop the registry and print the report.
	if ( (bool)usage["check-updates"] ) {
		const char* ud = getenv("DOCKER2UXC_UXCDIR");
		std::string report, err;
		docker2uxc::check_updates(ud ? ud : "/etc/uxc", auth_file, report, err);
		std::cout << report;
		return fin(0);
	}

	std::vector<std::string> rest = usage.remainder();

	// --resolve-digest: print just the digest the positional ref resolves to.
	if ( (bool)usage["resolve-digest"] ) {
		if ( rest.empty()) { logger::error << "docker2uxc: missing <image-ref>" << std::endl; return fin(1); }
		std::string err, dg = docker2uxc::resolve_digest(rest[0], auth_file, err);
		if ( dg.empty()) { logger::error << "docker2uxc: " << err << std::endl; return fin(1); }
		std::cout << dg << std::endl;
		return fin(0);
	}

	docker2uxc::Options o;
	o.auth_file = auth_file;
	if ( (bool)usage["dockerfile"] ) o.dockerfile = usage["dockerfile"].value;
	else if ( !rest.empty()) o.image = rest[0];
	else { logger::error << "docker2uxc: missing <image-ref>" << std::endl; std::cout << usage << std::endl; return fin(1); }

	if ( (bool)usage["context"] )  o.context = usage["context"].value;
	if ( (bool)usage["out"] )      o.out = usage["out"].value;
	if ( (bool)usage["name"] )     o.name = usage["name"].value;
	if ( (bool)usage["arch"] )     o.arch = usage["arch"].value;
	if ( (bool)usage["caps"] )     o.caps = usage["caps"].value;
	o.privileged       = (bool)usage["privileged"];
	o.network_isolated = ( (bool)usage["network"] && usage["network"].value == "isolated" );
	o.resolvconf       = (bool)usage["resolv-conf"];
	o.accounting       = !(bool)usage["no-accounting"];
	o.dev              = (bool)usage["dev"];
	o.rw_overlay       = (bool)usage["rw-overlay"] || o.dev;
	if ( (bool)usage["cntrinit"] ) o.cntrinit = usage["cntrinit"].value;
	if ( (bool)usage["profile"] )  o.profile = usage["profile"].value;
	o.emit_netconfig   = (bool)usage["emit-netconfig"];
	if ( (bool)usage["net-bridge"] ) o.net_bridge = usage["net-bridge"].value;
	o.emit_keeper      = (bool)usage["emit-keeper"];
	o.do_register      = !(bool)usage["no-register"];
	o.autostart        = (bool)usage["autostart"];
	if ( (bool)usage["infra"] )    o.infra = usage["infra"].value;
	o.verify           = !(bool)usage["no-verify"];
	o.force            = (bool)usage["force"];
	{ const char* ud = getenv("DOCKER2UXC_UXCDIR"); o.uxc_dir = ud ? ud : "/etc/uxc"; }
	{ const char* ce = getenv("DOCKER2UXC_CACHE");
	  o.cache_dir = (bool)usage["cache"] ? usage["cache"].value : ( ce ? ce : "/tmp/docker2uxc-cache" ); }

	std::string err;
	bool ok = docker2uxc::convert(o, err);
	if ( !ok ) logger::error << "docker2uxc: " << err << std::endl;
	return fin(ok ? 0 : 1);
}
