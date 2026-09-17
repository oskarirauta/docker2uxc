#pragma once
#include <string>
#include <vector>
#include "json.hpp"
#include "emit.hpp"
#include "convert.hpp"

// Recipes: a profile that also knows how to CREATE its container.
//
// A profile has always described how to RUN an application (caps, mounts, and
// the _registry half: volumes, devices, healthcheck). What it could not say is
// where the container comes from - so every deployment still started with the
// operator hand-writing a `uxc pull` or a Dockerfile, and a rebuild after a
// flash meant reconstructing both from memory.
//
// A recipe is that same profile plus a "_source" block:
//
//   "_source": { "image": "caddy:2.11-alpine" }             -> pull
//   "_source": { "build": { "base": "php:8.5-fpm-alpine",
//                           "dockerfile": [ "FROM ${base}", "RUN ..." ] } }
//
// Everything else (_seed, _paths, _registry) already existed or is a small
// addition, and applies exactly as it does for a plain profile. A profile
// WITHOUT _source stays what it always was - this is one mechanism with an
// optional half, not a second system beside profiles.
namespace recipe {

// A recipe resolved into something that can be converted: the same Options a
// `uxc pull`/`uxc build` would have been given, plus what the caller must do
// around it (create host paths, seed config files).
struct Plan {
	std::string name;                 // container name (the recipe name unless overridden)
	std::string recipe;               // the recipe this came from
	std::string description;
	bool        build = false;        // true: Dockerfile build, false: plain pull
	std::string image;                // pull mode: the ref to fetch
	std::string base;                 // build mode: the FROM ref
	std::string dockerfile_body;      // build mode: the composed Dockerfile text
	std::string dockerfile_path;      // build mode: where the body was written (filled by materialise)
	std::vector<emit::PathSpec> paths;
	JSON        seed = JSON::Object();
	std::string infra;                // _source.infra, if the recipe wants a shared netns
	bool        autostart = false;
};

// True when <dir>/<name>.json exists and carries a "_source" block, i.e. it can
// be deployed rather than only applied as an overlay at pull time.
bool is_recipe(const std::string& dir, const std::string& name);

// Names of every deployable recipe in `dir` (sorted). A plain profile is not
// listed - `emit::profile_names` still lists both.
std::vector<std::string> names(const std::string& dir);

// Resolve <dir>/<name>.json into a Plan. `container` overrides the container
// name (empty = the recipe name). Returns false + err when the recipe is
// missing, has no _source, or its _source is malformed.
bool plan(const std::string& dir, const std::string& name, const std::string& container,
          Plan& out, std::string& err);

// Write the plan's Dockerfile (build mode) next to the future bundle as
// <bundle_dir>/<name>.Dockerfile - the same editable recipe file `uxc build`
// and the LuCI wizard produce, so a recipe-built container is afterwards an
// ordinary Dockerfile container that can be edited and rebuilt by hand.
// Fills plan.dockerfile_path. No-op (true) in pull mode.
bool materialise(Plan& p, const std::string& bundle_dir, std::string& err);

// Fill a converter Options from a resolved plan: image/dockerfile, the recipe
// name (recorded as provenance), profile overlay (the recipe applies to itself),
// infra and autostart. Values the CALLER already set win, so an explicit
// --infra/--profile/--name on the command line overrides the recipe. The caller
// still sets out/uxc_dir/auth_file/cache_dir.
void apply(const Plan& p, docker2uxc::Options& o);

}
