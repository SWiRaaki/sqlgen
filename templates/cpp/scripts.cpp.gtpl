<<<
output: \(prefix)_scripts.cpp
when:   embed_scripts
>>>
\include("defs")
\call header("every script", null)

#include "\(prefix).hpp"

namespace \(prefix) {
\define patch_texts(p, patch) {
\for (s in p.scripts) {

\call ctext("text", patch, loop.index, s.text)
\}
\}
\define patch_scripts(p, patch) {

static const Script scripts_\(patch)[] = {
\for (s in p.scripts) {
	{ \(s.name | cstring), text_\(patch)_\(loop.index) },
\}
};
\}
\if (scripts.systables) {

\call ctext("systables", "text", null, scripts.systables.text)

static const Script systables_script = { \(scripts.systables.name | cstring), systables_text };

const Script *const systables = &systables_script;
\} \else {

const Script *const systables = nullptr;
\}
\for (p in scripts.patches) {
\call patch_texts(p, loop.index)
\call patch_scripts(p, loop.index)
\}
\if (scripts.patches | count) {

static const Patch patch_list[] = {
\for (p in scripts.patches) {
	{ \(p.version | cstring), \(p.major)u, \(p.minor)u, \(p.patch)u, \(p.prerelease | cstring), \(p.build | cstring),
	  scripts_\(loop.index), \(p.scripts | count) },
\}
};

const Patch *const patches = patch_list;
\} \else {

const Patch *const patches = nullptr;
\}

const size_t patch_count = \(scripts.patches | count);

const char schema_hash[] = \(hash | cstring);

} // namespace \(prefix)
