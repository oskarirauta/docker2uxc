#include "recipe.hpp"
#include "emit.hpp"
#include "logger.hpp"

#include <fstream>
#include <algorithm>
#include <sys/stat.h>

namespace recipe {
namespace {

// Join a JSON string-or-array-of-strings into one text block. A Dockerfile is
// far more readable as an array of lines inside JSON than as one string full of
// "\n", so both spellings are accepted.
std::string lines_to_text(const JSON& v) {
	if ( v.type() == JSON::TYPE::STRING ) {
		std::string s = v.to_string();
		if ( !s.empty() && s.back() != '\n' ) s += "\n";
		return s;
	}
	if ( v.type() != JSON::TYPE::ARRAY ) return "";
	std::string out;
	for ( auto it = v.begin(); it != v.end(); ++it ) out += it.value().to_string() + "\n";
	return out;
}

// Substitute ${base} (and nothing else) in a recipe's Dockerfile body. A recipe
// states its base image ONCE, in _source.build.base, because that is the value
// check_updates re-resolves and an upgrade follows - a FROM written out by hand
// in the body would drift away from it silently.
std::string expand_base(const std::string& body, const std::string& base) {
	const std::string tok = "${base}";
	std::string out;
	std::string::size_type pos = 0;
	for (;;) {
		std::string::size_type f = body.find(tok, pos);
		if ( f == std::string::npos ) { out += body.substr(pos); break; }
		out += body.substr(pos, f - pos) + base;
		pos = f + tok.size();
	}
	return out;
}

JSON load(const std::string& dir, const std::string& name, std::string& err) {
	std::string pf = dir + "/" + name + ".json";
	std::ifstream f(pf);
	if ( !f ) { err = "recipe not found: " + pf; return JSON::Object(); }
	std::string s(( std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	try {
		JSON j = JSON::parse(s);
		if ( j.type() != JSON::TYPE::OBJECT ) { err = "recipe " + name + ": not an object"; return JSON::Object(); }
		return j;
	} catch ( const std::exception& e ) {
		err = std::string("recipe ") + name + ": " + e.what();
		return JSON::Object();
	}
}

}

bool is_recipe(const std::string& dir, const std::string& name) {
	std::string err;
	JSON j = load(dir, name, err);
	if ( !err.empty()) return false;
	return j.contains("_source") && j["_source"].type() == JSON::TYPE::OBJECT;
}

std::vector<std::string> names(const std::string& dir) {
	std::vector<std::string> out;
	for ( const std::string& n : emit::profile_names(dir))
		if ( is_recipe(dir, n)) out.push_back(n);
	return out;   // profile_names() already sorted
}

bool plan(const std::string& dir, const std::string& name, const std::string& container,
          Plan& out, std::string& err) {
	JSON j = load(dir, name, err);
	if ( !err.empty()) return false;
	if ( !j.contains("_source") || j["_source"].type() != JSON::TYPE::OBJECT ) {
		err = "'" + name + "' is a profile, not a recipe: it has no \"_source\" saying where the container comes from";
		return false;
	}

	emit::ProfileInfo pi;
	if ( !emit::profile_info(dir, name, pi, err)) return false;

	out.name        = container.empty() ? name : container;
	out.recipe      = name;
	out.description = pi.description;
	out.paths       = pi.paths;
	out.seed        = pi.seed;

	JSON src = j["_source"];
	if ( src.contains("infra"))     out.infra     = src["infra"].to_string();
	if ( src.contains("autostart")) out.autostart = src["autostart"].to_bool();

	bool has_image = src.contains("image") && !src["image"].to_string().empty();
	bool has_build = src.contains("build") && src["build"].type() == JSON::TYPE::OBJECT;
	if ( has_image && has_build ) { err = "recipe " + name + ": _source has both 'image' and 'build' - pick one"; return false; }

	if ( has_image ) {
		out.build = false;
		out.image = src["image"].to_string();
		return true;
	}
	if ( !has_build ) { err = "recipe " + name + ": _source needs either 'image' or 'build'"; return false; }

	JSON b = src["build"];
	out.build = true;
	out.base  = b.contains("base") ? b["base"].to_string() : "";
	if ( out.base.empty()) { err = "recipe " + name + ": _source.build needs a 'base' image"; return false; }

	// The Dockerfile is either written inline (lines/string) or pointed at on
	// disk - the latter keeps a hand-maintained Dockerfile the single source of
	// truth and still lets the recipe carry everything around it.
	if ( b.contains("dockerfile_path") && !b["dockerfile_path"].to_string().empty()) {
		std::string p = b["dockerfile_path"].to_string();
		std::ifstream df(p);
		if ( !df ) { err = "recipe " + name + ": cannot read dockerfile_path " + p; return false; }
		out.dockerfile_body = std::string(( std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
	} else if ( b.contains("dockerfile")) {
		out.dockerfile_body = lines_to_text(b["dockerfile"]);
	}
	if ( out.dockerfile_body.empty()) { err = "recipe " + name + ": _source.build needs 'dockerfile' or 'dockerfile_path'"; return false; }
	out.dockerfile_body = expand_base(out.dockerfile_body, out.base);
	return true;
}

bool materialise(Plan& p, const std::string& bundle_dir, std::string& err) {
	if ( !p.build ) return true;
	std::string path = bundle_dir + "/" + p.name + ".Dockerfile";
	// A recipe deploy is also an upgrade path: the file is rewritten so an edited
	// recipe reaches the container, and check_updates' dockerfile_sha256 then
	// tracks THIS file - the thing `uxc build` would rebuild from.
	std::ofstream f(path);
	if ( !f ) { err = "cannot write " + path; return false; }
	f << "# Generated by `uxc deploy " << p.recipe << "`.\n"
	  << "# Edit freely: this file is the container's recipe from here on, and\n"
	  << "# `uxc upgrade " << p.name << "` rebuilds from it. Re-running deploy overwrites it.\n"
	  << p.dockerfile_body;
	if ( !f ) { err = "write error on " + path; return false; }
	f.close();
	p.dockerfile_path = path;
	return true;
}

void apply(const Plan& p, docker2uxc::Options& o) {
	if ( o.name.empty()) o.name = p.name;
	o.recipe = p.recipe;
	// The recipe is its own profile: its top-level half (caps, mounts) and its
	// _registry half are applied by the converter exactly as `--profile` does.
	// An explicit --profile still wins - the operator is allowed to disagree.
	if ( o.profile.empty()) o.profile = p.recipe;
	if ( p.build ) {
		o.dockerfile = p.dockerfile_path;
		std::string::size_type s = p.dockerfile_path.find_last_of('/');
		o.context = ( s == std::string::npos ) ? std::string(".") : p.dockerfile_path.substr(0, s);
	} else {
		o.image = p.image;
	}
	if ( o.infra.empty() && !p.infra.empty()) o.infra = p.infra;
	if ( p.autostart ) o.autostart = true;
	o.force = true;        // a deploy is idempotent: re-running it replaces the bundle
}

}
