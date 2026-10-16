/* The configuration: the written defaults agree with the built-in ones, a named table entry
 * replaces "*" key by key, rule levels start from their defaults, target directories default per
 * language and never nest, and JSON5 is refused. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"

static int   failures;
static char *dir;

static void expect(int condition, const char *what) {
	if (!condition) {
		printf("config: %s\n", what);
		++failures;
	}
}

static int same(const char *left, const char *right) {
	return (!left && !right) || (left && right && strcmp(left, right) == 0);
}

/* Write @p text to a file in the scratch directory and load it. */
static int load(const char *name, const char *text, sg_config *out) {
	char *path = sg_format("%s/%s", dir, name);
	int   ok   = sg_write_file(path, text, strlen(text)) && sg_config_load(path, out);

	free(path);
	return ok;
}

static void defaults_agree(void) {
	sg_config written;
	sg_config empty;

	if (!load("written.json", sg_config_default_text(), &written) || !load("empty.json", "{}", &empty)) {
		expect(0, "the written defaults or an empty configuration do not load");
		return;
	}

	expect(same(written.systables, empty.systables), "systables differ");
	expect(written.patch_patterns.count == 1 && empty.patch_patterns.count == 1
			&& same(written.patch_patterns.items[0], empty.patch_patterns.items[0]), "patches differ");
	expect(written.double_quoted_strings == empty.double_quoted_strings, "double_quoted_strings differs");
	expect(written.foreign_keys == empty.foreign_keys, "foreign_keys differs");
	expect(written.targets.count == 3 && empty.targets.count == 3, "the default targets differ");

	for (size_t i = 0; i < 3 && written.targets.count == 3 && empty.targets.count == 3; ++i) {
		const sg_target *w = &written.targets.items[i];
		const sg_target *e = &empty.targets.items[i];

		expect(same(w->language, e->language) && same(w->dir, e->dir) && same(w->prefix, e->prefix)
				&& same(w->templates, e->templates) && w->confirm_clear == e->confirm_clear
				&& w->types.count == e->types.count, "a key of a default target differs");
	}

	if (empty.targets.count == 3) {
		const sg_target *c  = &empty.targets.items[0];
		const sg_target *md = &empty.targets.items[1];
		const sg_target *mm = &empty.targets.items[2];

		expect(same(c->language, "c") && strstr(c->dir, "gen") && same(c->prefix, "db") && !c->templates
				&& c->confirm_clear && c->types.count == 0, "the default C target changed");
		expect(same(md->language, "markdown") && strstr(md->dir, "docs/schema") && !md->prefix,
				"the default markdown target changed");
		expect(same(mm->language, "mermaid") && strstr(mm->dir, "docs/diagram") && !mm->prefix,
				"the default mermaid target changed");
	}

	expect(written.embed_scripts == empty.embed_scripts, "embed_scripts differs");
	expect(written.typed_ids == empty.typed_ids, "typed_ids differs");
	expect(written.types_by_name.count == 0 && empty.types_by_name.count == 0, "types.by_name not empty");
	expect(written.types_by_declared.count == 0 && empty.types_by_declared.count == 0,
			"types.by_declared not empty");
	expect(written.tables.count == 0 && empty.tables.count == 0, "tables not empty");
	expect(written.queries.count == 0 && empty.queries.count == 0, "queries not empty");
	expect(written.rules.count == empty.rules.count && empty.rules.count > 0, "rule counts differ");

	for (size_t i = 0; i < written.rules.count && i < empty.rules.count; ++i) {
		expect(strcmp(written.rules.items[i].name, empty.rules.items[i].name) == 0
				&& written.rules.items[i].level == empty.rules.items[i].level,
				"a rule's written level differs from its default");

		const sg_rule_config *w = &written.rules.items[i];
		const sg_rule_config *e = &empty.rules.items[i];

		expect(w->params.count == e->params.count, "a rule's written parameters differ from its defaults");

		for (size_t p = 0; p < w->params.count && p < e->params.count; ++p) {
			expect(strcmp(w->params.items[p].key, e->params.items[p].key) == 0
					&& same(w->params.items[p].value, e->params.items[p].value),
					"a rule's written parameter differs from its default");
		}
	}

	/* The default values themselves, so changing one has to change this test as well. */
	expect(!empty.systables, "systables has a default");
	expect(empty.patch_patterns.count == 1 && strstr(empty.patch_patterns.items[0], "sql/*/") != NULL,
			"patches is not sql/*/");
	expect(!empty.double_quoted_strings && empty.foreign_keys, "schema flags changed");
	expect(empty.embed_scripts && empty.typed_ids, "generate flags changed");
	expect(sg_config_rule(&empty, "fk_target")->level == SG_LEVEL_ERROR
			&& sg_config_rule(&empty, "fk_indexed")->level == SG_LEVEL_WARN
			&& sg_config_rule(&empty, "nullable_unique")->level == SG_LEVEL_WARN
			&& sg_config_rule(&empty, "pk_naming")->level == SG_LEVEL_OFF, "rule defaults changed");

	sg_config_free(&written);
	sg_config_free(&empty);
}

static void tables_replace_key_by_key(void) {
	sg_config config;

	if (!load("tables.json", "{ \"tables\": {"
			" \"*\": { \"writes\": [\"insert\", \"delete\"] },"
			" \"users\": { \"writes\": [\"update\"] },"
			" \"sessions\": { \"writes\": false } } }", &config)) {
		expect(0, "the tables example does not load");
		return;
	}

	sg_table_options chapter  = sg_config_table(&config, "chapter");
	sg_table_options users    = sg_config_table(&config, "users");
	sg_table_options upper    = sg_config_table(&config, "USERS");
	sg_table_options sessions = sg_config_table(&config, "sessions");

	expect(chapter.has_writes && chapter.writes == (SG_WRITE_INSERT | SG_WRITE_DELETE),
			"a table without an entry does not get \"*\"");
	expect(users.writes == SG_WRITE_UPDATE, "a named entry was combined with \"*\" instead of replacing it");
	expect(upper.writes == SG_WRITE_UPDATE, "table names are not matched case-insensitively");
	expect(sessions.has_writes && sessions.writes == 0, "writes: false did not take writes away");

	sg_config_free(&config);

	if (load("none.json", "{}", &config)) {
		expect(!sg_config_table(&config, "users").has_writes, "a table without any entry has writes");
	} else {
		expect(0, "an empty configuration does not load");
	}

	sg_config_free(&config);
}

static void rules_start_from_defaults(void) {
	sg_config config;

	if (!load("rules.json", "{ \"rules\": { \"fk_indexed\": \"error\","
			" \"pk_naming\": { \"level\": \"warn\", \"plain\": \"key\" } } }", &config)) {
		expect(0, "the rules example does not load");
		return;
	}

	const sg_rule_config *indexed = sg_config_rule(&config, "fk_indexed");
	const sg_rule_config *naming  = sg_config_rule(&config, "pk_naming");
	const sg_rule_config *target  = sg_config_rule(&config, "fk_target");

	expect(indexed->level == SG_LEVEL_ERROR && indexed->params.count == 0, "a level string was not applied");
	expect(naming->level == SG_LEVEL_WARN, "a rule object lost its level");
	expect(strcmp(sg_rule_param(naming, "plain"), "key") == 0, "a parameter given was not applied");
	expect(strcmp(sg_rule_param(naming, "autoincrement"), "serial") == 0, "a parameter left out lost its default");
	expect(!sg_rule_param(sg_config_rule(&config, "prefix_origin"), "prefix"), "prefix_origin.prefix has a default");
	expect(target->level == SG_LEVEL_ERROR, "an unmentioned rule lost its default");
	expect(!sg_config_rule(&config, "no_such_rule"), "an unknown rule was found");

	sg_config_free(&config);
}

/* A target without a directory gets its language's own; none may write inside another's. */
static void target_directories(void) {
	sg_config config;

	expect(load("dirs.json", "{ \"targets\": [{ \"language\": \"c\" }, { \"language\": \"markdown\" },"
			" { \"language\": \"mermaid\", \"dir\": \"./docs//diagram/\" }] }", &config),
			"targets of different languages without a directory do not load");

	if (config.targets.count == 3) {
		expect(strstr(config.targets.items[0].dir, "/gen") != NULL, "the C target's directory is not gen");
		expect(strstr(config.targets.items[1].dir, "/docs/schema") != NULL, "the markdown directory is not docs/schema");
		expect(strstr(config.targets.items[2].dir, "/docs/diagram") != NULL
				&& strstr(config.targets.items[2].dir, "//") == NULL && strstr(config.targets.items[2].dir, "/./") == NULL,
				"a target directory is not normalised");
	}

	sg_config_free(&config);

	expect(!load("nested.json", "{ \"targets\": [{ \"language\": \"markdown\", \"dir\": \"gen/docs\" },"
			" { \"language\": \"c\", \"dir\": \"gen\" }] }", &config), "a target inside another's directory loads");
	sg_config_free(&config);
	expect(!load("same.json", "{ \"targets\": [{ \"language\": \"markdown\", \"dir\": \"out\" },"
			" { \"language\": \"mermaid\", \"dir\": \"./out/\" }] }", &config), "two targets in one directory load");
	sg_config_free(&config);
	expect(load("siblings.json", "{ \"targets\": [{ \"language\": \"markdown\", \"dir\": \"gen-docs\" },"
			" { \"language\": \"c\", \"dir\": \"gen\" }] }", &config), "a sibling whose name extends another's is refused");
	sg_config_free(&config);
}

static void strict_json_only(void) {
	sg_config config;

	/* Valid JSON5, which SQLite alone would accept. */
	expect(!load("json5.json", "{ \"schema\": { patches: \"sql/*/\" } }", &config),
			"an unquoted key was accepted");
	sg_config_free(&config);

	expect(load("null.json", "{ \"schema\": { \"systables\": null } }", &config) && !config.systables,
			"systables: null is not the same as leaving it out");
	sg_config_free(&config);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fputs("usage: config_test <scratch directory>\n", stderr);
		return 2;
	}

	dir = sg_format("%s/config_test.scratch", argv[1]);
	mkdir(dir, 0777);

	defaults_agree();
	tables_replace_key_by_key();
	rules_start_from_defaults();
	target_directories();
	strict_json_only();

	free(dir);
	printf("config: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
