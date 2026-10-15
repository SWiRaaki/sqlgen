/* The template language: what each form renders, the blank-line rule, alignment, headers, and
 * every error with its position. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "template.h"
#include "value.h"

static int    failures;
static sg_buf messages;

static void collect(const char *line, void *user) {
	(void)user;
	sg_buf_puts(&messages, line);
	sg_buf_putc(&messages, '\n');
}

/* One template named "t.gtpl" plus optional partials, rendered against JSON data. */
typedef struct {
	const char *name;
	const char *text;
} source;

static sg_value *data_of(const char *json) {
	sg_value *data = sg_value_from_json(json, strlen(json));

	if (!data) {
		printf("template: test data is not JSON: %s\n", json);
		exit(1);
	}

	return data;
}

static bool run(const source *sources, size_t count, const char *json, sg_rendered *out) {
	sg_template_source list[8];

	for (size_t i = 0; i < count; ++i) {
		list[i].name = sources[i].name;
		list[i].text = sources[i].text;
	}

	sg_template_set *set  = sg_templates_parse(list, count, NULL);
	sg_value        *data = data_of(json);
	bool             ok   = set && sg_templates_render(set, data, NULL, 0, out);

	sg_templates_free(set);
	sg_value_free(data);
	return ok;
}

/* The file @p path the set produces must be exactly @p expected. */
static void renders_set(const char *label, const source *sources, size_t count, const char *json,
		const char *path, const char *expected) {
	sg_rendered out = {0};

	sg_buf_free(&messages);

	if (!run(sources, count, json, &out)) {
		printf("template: %s: failed:\n%s", label, messages.data ? messages.data : "");
		++failures;
		return;
	}

	const char *got = NULL;

	for (size_t i = 0; i < out.count; ++i) {
		if (strcmp(out.items[i].path, path) == 0) {
			got = out.items[i].text.data;
		}
	}

	if (!got) {
		printf("template: %s: no file %s\n", label, path);
		++failures;
	} else if (strcmp(got, expected) != 0) {
		printf("template: %s:\n--- expected\n%s--- got\n%s---\n", label, expected, got);
		++failures;
	}

	sg_rendered_free(&out);
}

/* A single template producing "out.txt". */
static void renders(const char *label, const char *body, const char *json, const char *expected) {
	char   *text = sg_format("<<<\noutput: out.txt\n>>>\n%s", body);
	source  one  = { "t.gtpl", text };

	renders_set(label, &one, 1, json, "out.txt", expected);
	free(text);
}

/* The set must fail with a message containing @p expected (which includes the position). */
static void fails_set(const char *label, const source *sources, size_t count, const char *json,
		const char *expected) {
	sg_rendered out = {0};

	sg_buf_free(&messages);

	if (run(sources, count, json, &out)) {
		printf("template: %s: rendered, expected \"%s\"\n", label, expected);
		++failures;
		sg_rendered_free(&out);
	} else if (!messages.data || !strstr(messages.data, expected)) {
		printf("template: %s: expected \"%s\", got:\n%s", label, expected, messages.data ? messages.data : "(nothing)\n");
		++failures;
	}
}

static void fails(const char *label, const char *body, const char *json, const char *expected) {
	char   *text = sg_format("<<<\noutput: out.txt\n>>>\n%s", body);
	source  one  = { "t.gtpl", text };

	fails_set(label, &one, 1, json, expected);
	free(text);
}

static const char *const TABLE = "{ \"prefix\": \"db\", \"t\": { \"name\": \"users\", \"columns\": ["
		"{ \"name\": \"serial\", \"type\": \"db_users_serial\", \"nullable\": false },"
		"{ \"name\": \"username\", \"type\": \"db_text\", \"nullable\": false },"
		"{ \"name\": \"deactivated_at\", \"type\": \"int64_t\", \"nullable\": true } ] } }";

int main(void) {
	sg_set_error_sink(collect, NULL);

	/* Plain output, including everything a backslash means in C. */
	renders("plain text", "a \"\\n\\t\\\\\" b\n#define X 1 \\\n\t+ 2\nchar f = '\\f'; /* \\format */\n", "{}",
			"a \"\\n\\t\\\\\" b\n#define X 1 \\\n\t+ 2\nchar f = '\\f'; /* \\format */\n");
	renders("no header needed for text", "x\n", "{}", "x\n");

	/* Expressions and filters. */
	renders("paths", "\\(t.name) \\(t.columns[1].name) \\(t.columns | count)\n", TABLE, "users username 3\n");
	renders("cases of a text", "\\(\"tokenHash\" | snake) \\(\"token_hash\" | pascal) \\(\"token hash\" | camel)"
			" \\(\"token_hash\" | upper) \\(\"MiXed\" | lower)\n", "{}",
			"token_hash TokenHash tokenHash TOKEN_HASH mixed\n");
	renders("cases of words", "\\(w | snake) \\(w | pascal)\n", "{ \"w\": [\"get\", \"by\", \"token\", \"hash\"] }",
			"get_by_token_hash GetByTokenHash\n");
	renders("at", "\\(xs | at(1))\n", "{ \"xs\": [\"a\", \"b\"] }", "b\n");
	{
		source two[] = { { "a.gtpl", "<<<\noutput: a.txt\nwhen: on\n>>>\nA\n" }, { "b.gtpl", "<<<\noutput: b.txt\nwhen: not on\n>>>\nB\n" } };

		renders_set("when", two, 2, "{ \"on\": true }", "a.txt", "A\n");
		sg_rendered skipped = {0};

		if (!run(two, 1, "{ \"on\": false }", &skipped) || skipped.count != 0) {
			printf("template: when false still produced %zu files\n", skipped.count);
			++failures;
		}

		sg_rendered_free(&skipped);
	}
	renders("join, first, last, default", "\\(xs | join(\", \")) \\(xs | first) \\(xs | last) \\(n | default(\"none\"))\n",
			"{ \"xs\": [\"a\", \"b\", 3], \"n\": null }", "a, b, 3 a 3 none\n");
	renders("strings with escapes", "\\(\"q\\\"\\\\(\")\n", "{}", "q\"\\(\n");
	renders("comparisons", "\\if (a == \"x\" and not (b != 2) or false) {yes\\}\n", "{ \"a\": \"x\", \"b\": 2 }", "yes\n");
	renders("ordering", "\\(1 < 2) \\(2 <= 2) \\(3 > 4) \\(xs | count >= 2)\n", "{ \"xs\": [1, 2] }", "true true false true\n");
	fails("ordering needs integers", "\\(\"a\" < 2)\n", "{}", "t.gtpl:4:7: error: <, <=, > and >= compare integers, not text and an integer");
	renders("booleans print", "\\(true) \\(1 == 2)\n", "{}", "true false\n");

	/* Loops, with their loop variables and nesting. */
	renders("loop with separators", "(\\for (c in t.columns) {\\(c.name)\\if (not loop.last) {, \\}\\})\n", TABLE,
			"(serial, username, deactivated_at)\n");
	renders("loop variables", "\\for (x in xs) {\\(loop.index)/\\(loop.number)/\\(loop.count)/\\(loop.first) \\}\n",
			"{ \"xs\": [1, 2] }", "0/1/2/true 1/2/2/false \n");
	renders("nested loops shadow loop", "\\for (a in xs) {\\for (b in xs) {\\(loop.index)\\}\\(loop.index);\\}\n",
			"{ \"xs\": [1, 2] }", "010;011;\n");
	renders("empty loop", "[\\for (x in xs) {\\(x)\\}]\n", "{ \"xs\": [] }", "[]\n");

	/* Conditions. */
	renders("if chain", "\\for (n in xs) {\\if (n == 1) {one\\} \\elif (n == 2) {two\\} \\else {many\\} \\}\n",
			"{ \"xs\": [1, 2, 3] }", "one two many \n");
	renders("truthiness", "\\if (s) {s\\}\\if (e) {e\\}\\if (z) {z\\}\\if (l) {l\\}\\if (n) {n\\}\\if (m) {m\\}.\n",
			"{ \"s\": \"x\", \"e\": \"\", \"z\": 0, \"l\": [], \"n\": null, \"m\": {} }", "s.\n");

	/* The blank-line rule: tag-only lines vanish, lines with content stay. */
	renders("tag-only lines vanish",
			"struct row {\n"
			"\\for (c in t.columns) {\n"
			"\tint \\(c.name);\n"
			"\\if (c.nullable) {\n"
			"\tbool has_\\(c.name);\n"
			"\\}\n"
			"\\}\n"
			"};\n", TABLE,
			"struct row {\n\tint serial;\n\tint username;\n\tint deactivated_at;\n\tbool has_deactivated_at;\n};\n");
	renders("indented tags vanish too", "a\n\t\\if (true) {   \n\tb\n  \\}\nc\n", "{}", "a\n\tb\nc\n");
	renders("a tag beside text keeps its line", "x \\if (true) {y\\}\nz\n", "{}", "x y\nz\n");
	renders("an expression alone keeps its line", "\\(t.name)\n", TABLE, "users\n");
	renders("comments", "a\\* inline *\\b\n\\* a comment\n   on two lines *\\\nc\n", "{}", "ab\nc\n");

	/* Alignment. */
	renders("align a struct",
			"typedef struct {\n"
			"\\align {\n"
			"\\for (c in t.columns) {\n"
			"\t\\(c.type)\\|\\(c.name);\n"
			"\\if (c.nullable) {\n"
			"\tbool\\|has_\\(c.name);\n"
			"\\}\n"
			"\\}\n"
			"\\}\n"
			"} \\(prefix)_\\(t.name)_row;\n", TABLE,
			"typedef struct {\n"
			"\tdb_users_serial serial;\n"
			"\tdb_text         username;\n"
			"\tint64_t         deactivated_at;\n"
			"\tbool            has_deactivated_at;\n"
			"} db_users_row;\n");
	renders("align several columns, lines without marks untouched",
			"\\align {\na\\|bb\\|c\nccc\\|d\\|e\n// note\n\\}\n", "{}", "a   bb c\nccc d  e\n// note\n");
	renders("align counts characters, not bytes", "\\align {\n\xc3\xa9\\|x\nab\\|y\n\\}\n", "{}", "\xc3\xa9  x\nab y\n");

	/* define, call and include. */
	{
		source set[] = {
			{ "t.gtpl", "<<<\noutput: out.txt\n>>>\n\\include(\"head\")\n\\define field(c, suffix) {\\(c.name)\\(suffix)\\}\n"
				"\\for (c in t.columns) {\n\\call field(c, \";\")\n\n\\}\n" },
			{ "head.gtpl", "/* \\(t.name) */\n" }
		};

		renders_set("include and call", set, 2, TABLE, "out.txt", "/* users */\nserial;\nusername;\ndeactivated_at;\n");
	}
	{
		source set[] = {
			{ "t.gtpl", "<<<\noutput: out.txt\n>>>\n\\define sep() { \\}\n\\define item(x) {\n\t<\\(x)>\n\\}\n"
				"a\\call sep()b\n\\for (x in xs) {\n\\call item(x)\n\\}\n" }
		};

		renders_set("define bodies keep their own text", set, 1, "{ \"xs\": [1, 2] }", "out.txt", "a b\n\t<1>\n\t<2>\n");
	}

	/* Headers: remapping, and one file per item. */
	renders("literal forms through strings", "\\(\"\\\\(x)\") \\(\"\\\\}\")\n", "{}", "\\(x) \\}\n");
	{
		source one[] = { { "t.gtpl", "<<<\n  sigil: @\n  expr: [ ]\n  block: < >\n  output: @[t.name].h\n>>>\n"
				"@for [c in t.columns] <@[c.name]@if [c.nullable] <?@>,@>\n\\(not a form)\n" } };

		renders_set("remapped sigil and brackets", one, 1, TABLE, "users.h",
				"serial,username,deactivated_at?,\n\\(not a form)\n");
	}
	{
		source one[] = { { "t.gtpl", "<<<\noutput: \\(prefix)_\\(c.name | snake).h\neach: c in t.columns\n>>>\n\\(c.type)\n" } };

		renders_set("each, first file", one, 1, TABLE, "db_serial.h", "db_users_serial\n");
		renders_set("each, last file", one, 1, TABLE, "db_deactivated_at.h", "int64_t\n");
	}
	{
		source two[] = { { "a.gtpl", "<<<\noutput: a.txt\n>>>\nA\n" }, { "part.gtpl", "only included\n" } };

		renders_set("a template without output produces nothing itself", two, 2, "{}", "a.txt", "A\n");
	}

	/* Errors, each with its position. */
	fails("unknown name", "x\n  \\(nope)\n", "{}", "t.gtpl:5:5: error: unknown name 'nope'");
	fails("missing member", "\\(t.nme)\n", TABLE, "t.gtpl:4:3: error: t has no member 'nme'");
	fails("member of a list", "\\(t.columns.name)\n", TABLE, "t.columns is a list, which has no member 'name'");
	fails("index out of range", "\\(t.columns[3].name)\n", TABLE, "t.columns[3] is out of range: t.columns holds 3 items");
	fails("print null", "\\(n)\n", "{ \"n\": null }", "t.gtpl:4:1: error: the value is null, which cannot be printed");
	fails("print a list", "\\(t.columns)\n", TABLE, "the value is a list, which cannot be printed");
	fails("unknown filter", "\\(t.name | shout)\n", TABLE, "t.gtpl:4:12: error: unknown filter 'shout'");
	fails("filter misuse", "\\(t.columns | lower)\n", TABLE, "lower takes a text, not a list");
	fails("loop over a map", "\\for (x in t) {\\}\n", TABLE, "t.gtpl:4:1: error: a loop goes over a list, not a map");
	fails("unclosed block", "a\n\\if (true) {\nb\n", "{}", "t.gtpl:5:1: error: this block is never closed with \\}");
	fails("stray close", "a \\}\n", "{}", "t.gtpl:4:3: error: this \\} closes no block");
	fails("else without if", "\\else {x\\}\n", "{}", "t.gtpl:4:1: error: this \\else follows no \\if");
	fails("mark outside align", "a\\|b\n", "{}", "t.gtpl:4:2: error: \\| marks a column, so it belongs inside \\align");
	fails("for without block", "\\for (x in xs) x\n", "{}", "t.gtpl:4:1: error: \\for must be followed by '{'");
	fails("if without condition", "\\if {x\\}\n", "{}", "t.gtpl:4:1: error: \\if must be followed by '('");
	fails("unclosed expression", "\\(t.name\n", "{}", "t.gtpl:4:2: error: this '(' is never closed");
	fails("unclosed comment", "\\* forever\n", "{}", "t.gtpl:4:1: error: this comment is never closed with '*\\'");
	fails("bad expression", "\\(a +)\n", "{}", "t.gtpl:4:5: error: '+' after the end of the expression");
	fails("bad loop head", "\\for (x of xs) {\\}\n", "{}", "t.gtpl:4:9: error: 'in' expected after the loop variable");
	fails("include needs a string", "\\include(x)\n", "{}", "\\include names a template as a string");
	fails("include a missing template", "\\include(\"nope\")\n", "{}", "t.gtpl:4:1: error: no template named 'nope' in this set");
	fails("call undefined", "\\call nope()\n", "{}", "t.gtpl:4:1: error: no nope is defined");
	fails("call arity", "\\define f(a) {\\}\n\\call f(1, 2)\n", "{}", "t.gtpl:5:1: error: f takes 1 arguments, not 2");
	fails("define twice", "\\define f() {\\}\n\\define f() {\\}\n", "{}", "t.gtpl:5:1: error: f is defined twice");
	{
		source loop[] = { { "t.gtpl", "<<<\noutput: out.txt\n>>>\n\\include(\"t\")\n" } };

		fails_set("endless include", loop, 1, "{}", "includes and calls nested more than 32 deep");
	}

	/* Header errors. */
	{
		source bad[] = { { "t.gtpl", "<<<\noutput: x\ncolour: red\n>>>\n" } };
		fails_set("unknown header key", bad, 1, "{}", "t.gtpl:3:1: error: unknown header key 'colour'");
	}
	{
		source bad[] = { { "t.gtpl", "<<<\nsigil: (\n>>>\n" } };
		fails_set("sigil is a bracket", bad, 1, "{}", "the sigil '(' is also one of the brackets");
	}
	{
		source bad[] = { { "t.gtpl", "<<<\nblock: ( (\n>>>\n" } };
		fails_set("bracket pair", bad, 1, "{}", "t.gtpl:2:1: error: block is an opening and a closing character");
	}
	{
		source bad[] = { { "t.gtpl", "<<<\noutput: x\n" } };
		fails_set("unclosed header", bad, 1, "{}", "t.gtpl:1:1: error: this header is never closed with a line '>>>'");
	}
	{
		source bad[] = { { "t.gtpl", "<<<\neach: c in xs\n>>>\n" } };
		fails_set("each without output", bad, 1, "{}", "each needs an output");
	}
	{
		source bad[] = { { "t.gtpl", "<<<\noutput: ../\\(x).h\n>>>\n" } };
		fails_set("output leaves the directory", bad, 1, "{ \"x\": \"a\" }", "output '../a.h' must be a relative path");
	}
	{
		source bad[] = { { "t.gtpl", "<<<\noutput: same.h\neach: x in xs\n>>>\n" } };
		fails_set("output produced twice", bad, 1, "{ \"xs\": [1, 2] }", "output 'same.h' is produced twice");
	}
	{
		source bad[] = { { "t.gtpl", "<<<\noutput: \\if (x) {a\\}\n>>>\n" } };
		fails_set("output with a block", bad, 1, "{}", "output holds only text and \\(…)");
	}

	sg_buf_free(&messages);
	printf("template: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
