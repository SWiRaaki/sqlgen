<<<
output: \(prefix)_scripts.c
when:   embed_scripts
>>>
\include("defs")
\call header("every script", null)

#include "\(prefix).h"
\define patch_texts(p, patch) {
\for (s in p.scripts) {

\call ctext("text", patch, loop.index, s.text)
\}
\}
\define patch_scripts(p, patch) {

static const \(prefix)_script scripts_\(patch)[] = {
\for (s in p.scripts) {
	{ \(s.name | cstring), text_\(patch)_\(loop.index) },
\}
};
\}
\if (scripts.systables) {

\call ctext("systables", "text", null, scripts.systables.text)

static const \(prefix)_script systables = { \(scripts.systables.name | cstring), systables_text };

const \(prefix)_script *const \(prefix)_systables = &systables;
\} \else {

const \(prefix)_script *const \(prefix)_systables = NULL;
\}
\for (p in scripts.patches) {
\call patch_texts(p, loop.index)
\call patch_scripts(p, loop.index)
\}
\if (scripts.patches | count) {

static const \(prefix)_patch patches[] = {
\for (p in scripts.patches) {
	{ \(p.version | cstring), \(p.major)u, \(p.minor)u, \(p.patch)u, \(p.prerelease | cstring), \(p.build | cstring),
	  scripts_\(loop.index), \(p.scripts | count) },
\}
};

const \(prefix)_patch *const \(prefix)_patches = patches;
\} \else {

const \(prefix)_patch *const \(prefix)_patches = NULL;
\}

const size_t \(prefix)_patch_count = \(scripts.patches | count);

const char \(prefix)_schema_hash[] = \(hash | cstring);
