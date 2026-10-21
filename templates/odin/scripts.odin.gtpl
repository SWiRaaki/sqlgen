<<<
output: scripts.odin
when:   embed_scripts
>>>
\include("defs")
\call header("every script", null)
package \(prefix)

/* One schema script, its text exactly as it runs. */
Script :: struct {
	name: cstring,
	sql:  cstring,
}

/* One patch: its version, parsed, and its scripts in the order they run. */
Patch :: struct {
	version:    string,
	major:      u64,
	minor:      u64,
	patch:      u64,
	prerelease: string,
	build:      string,
	scripts:    []Script,
}
\if (scripts.systables) {

\call otext("systables", scripts.systables.text)

/* The script creating the system tables, run before every patch; nil when there is none. */
systables: Maybe(Script) = Script{\(scripts.systables.name | odinstring), SQL_systables}
\} \else {

/* The script creating the system tables, run before every patch; nil when there is none. */
systables: Maybe(Script)
\}

/* Every patch, in the order of their versions' precedence. */
patches := []Patch{
\for (p in scripts.patches) {
	{
		version = \(p.version | odinstring),
		major = \(p.major),
		minor = \(p.minor),
		patch = \(p.patch),
		prerelease = \(p.prerelease | odinstring),
		build = \(p.build | odinstring),
		scripts = {
\for (s in p.scripts) {
			{\(s.name | odinstring), \(s.text | odinstrings | join(" +\n\t\t\t\t"))},
\}
		},
	},
\}
}

/* A hash of every script's name and text, in order: it changes whenever the schema does. */
schema_hash :: \(hash | odinstring)
