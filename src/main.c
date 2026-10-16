#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "config.h"
#include "diff.h"
#include "model.h"
#include "plan.h"
#include "query.h"
#include "rules.h"
#include "schema.h"
#include "generate.h"
#include "util.h"
#include "version.h"


enum {
	EXIT_INPUT = 1,   /* The input has problems; they were reported. */
	EXIT_USAGE = 2
};

typedef struct {
	const char *command;
	const char *args[2];        /* positional arguments after the command */
	size_t      arg_count;
	const char *config;
	bool        config_given;   /* -c was used, so a missing file is a mistake, not a first run */
	const char *output;
	const char *database;
	const char *key;
	bool        strict;         /* --strict: warnings are errors */
	bool        yes;            /* --yes: clear output directories without asking */
	const char *from;           /* diff: the older patch */
	const char *to;             /* diff: the newer patch */
	bool        json;           /* diff: JSON instead of lines */
} options;

static void usage(FILE *to) {
	fputs("usage: sqlgen <command> [options]\n"
			"\n"
			"commands:\n"
			"  init      write a configuration with every default (sqlgen.json, or -c)\n"
			"  model     load the schema and print its model as JSON\n"
			"  check     load the schema and check it against the rules; writes nothing\n"
			"  plan      check the schema, then print what would be generated as JSON\n"
			"  report    print the model of an existing database as JSON, and check it\n"
			"            (rule levels from -c when given, else the defaults)\n"
			"  generate  check the schema, then write every target's files\n"
			"  diff      what changed: --from <version> [--to <version>] between two patches,\n"
			"            or --db <file> between a database and the scripts at its patch;\n"
			"            exits 0 when nothing differs, 1 when something does, 2 on errors\n"
			"  templates <language> <dir>\n"
			"            write a language's built-in templates into dir, to copy and change\n"
			"\n"
			"options:\n"
			"  -c, --config <file>  configuration (default: sqlgen.json, written with every\n"
			"                       default when it does not exist)\n"
			"  -o, --output <file>  write to a file instead of standard output\n"
			"      --db <file>      the database to report on or compare\n"
			"      --from, --to <version>\n"
			"                       the patches diff compares; --to defaults to the newest\n"
			"      --json           diff: print the changes as JSON\n"
			"      --key <key>      its encryption key (needs a build with CIPHER=1)\n"
			"      --strict         treat rule warnings as errors\n"
			"      --yes            clear output directories without asking\n"
			"  -h, --help           this text\n"
			"      --version        sqlgen and SQLite versions\n", to);
}

static bool parse(int argc, char **argv, options *out) {
	memset(out, 0, sizeof *out);
	out->config = SG_CONFIG_DEFAULT_PATH;

	for (int i = 1; i < argc; ++i) {
		const char *arg   = argv[i];
		bool        value = i + 1 < argc;

		if ((strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) && value) {
			out->config       = argv[++i];
			out->config_given = true;
		} else if ((strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) && value) {
			out->output = argv[++i];
		} else if (strcmp(arg, "--db") == 0 && value) {
			out->database = argv[++i];
		} else if (strcmp(arg, "--key") == 0 && value) {
			out->key = argv[++i];
		} else if (strcmp(arg, "--from") == 0 && value) {
			out->from = argv[++i];
		} else if (strcmp(arg, "--to") == 0 && value) {
			out->to = argv[++i];
		} else if (strcmp(arg, "--json") == 0) {
			out->json = true;
		} else if (strcmp(arg, "--strict") == 0) {
			out->strict = true;
		} else if (strcmp(arg, "--yes") == 0) {
			out->yes = true;
		} else if (arg[0] == '-') {
			sg_error(NULL, 0, 0, "unknown option or missing value: %s", arg);
			return false;
		} else if (!out->command) {
			out->command = arg;
		} else if (out->arg_count < 2) {
			out->args[out->arg_count++] = arg;
		} else {
			sg_error(NULL, 0, 0, "unexpected argument: %s", arg);
			return false;
		}
	}

	if (!out->command) {
		sg_error(NULL, 0, 0, "no command given");
		return false;
	}

	return true;
}

static bool emit(const options *opts, const sg_buf *text) {
	if (opts->output) {
		return sg_write_file(opts->output, text->data, text->len);
	}

	return fwrite(text->data, 1, text->len, stdout) == text->len && fflush(stdout) == 0;
}

static sqlite3 *open_scratch(void) {
	sqlite3 *db = NULL;

	if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		sg_error(NULL, 0, 0, "a scratch database could not be opened: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}

	return db;
}

static const char *plural(size_t count, const char *one, const char *many) {
	return count == 1 ? one : many;
}

/* After a run the rules failed: how many findings, so the end of a long list says what it adds up to. */
static void failed(const sg_rules_result *result) {
	fprintf(stderr, "sqlgen: %u %s", result->errors, plural(result->errors, "error", "errors"));

	if (result->warnings) {
		fprintf(stderr, " and %u %s", result->warnings, plural(result->warnings, "warning", "warnings"));
	}

	fputc('\n', stderr);
}

static int run_init(const options *opts) {
	const char *text = sg_config_default_text();

	if (!sg_create_file(opts->config, text, strlen(text))) {
		return EXIT_INPUT;
	}

	printf("wrote %s\n", opts->config);
	return 0;
}

/*
 * No -c and no sqlgen.json: write one with every default and stop, so nothing is generated from
 * settings nobody has read. The run fails, so a build that lost its configuration fails visibly.
 * With -c, a missing file is an error like any unreadable one.
 */
static bool first_run(const options *opts) {
	if (opts->config_given || sg_exists(opts->config)) {
		return false;
	}

	const char *text = sg_config_default_text();

	if (sg_create_file(opts->config, text, strlen(text))) {
		sg_error(opts->config, 0, 0, "did not exist, so it was written with every default;"
				" review it, then run again");
	}

	return true;
}

typedef enum {
	MODE_MODEL,   /* print the model */
	MODE_CHECK,   /* run the rules */
	MODE_PLAN,    /* run the rules, then print the plan */
	MODE_GENERATE /* run the rules, then generate */
} mode;

static int run_schema(const options *opts, mode what) {
	sg_config   config  = {0};
	sg_patchset patches = {0};
	sg_model    model   = {0};
	sqlite3    *db      = NULL;
	sg_queries  queries = {0};
	int         status  = EXIT_INPUT;

	if (first_run(opts)) {
		return EXIT_INPUT;
	}

	if (!sg_config_load(opts->config, &config) || !sg_patches_find(&config, &patches)) {
		goto done;
	}

	db = open_scratch();

	if (!db || !sg_schema_load(db, &config, &patches) || !sg_introspect(db, &model)) {
		goto done;
	}

	sg_schema_attach_origins(&patches, &model);

	/* The model is the schema alone; everything else also checks the queries against it. */
	if (what != MODE_MODEL && !sg_queries_load(db, &model, &config, &queries)) {
		goto done;
	}

	if (what == MODE_MODEL) {
		sg_buf json = {0};

		sg_model_write_json(&model, &patches, NULL, &json);
		status = emit(opts, &json) ? 0 : EXIT_INPUT;
		sg_buf_free(&json);
	} else if (what == MODE_GENERATE) {
		sg_rules_input      input   = { db, &model, &config, NULL, opts->strict, &queries, &patches };
		sg_rules_result     result;
		sg_plan             plan    = {0};
		sg_generate_options settings = { opts->config, opts->yes };

		sg_rules_run(&input, &result);

		if (result.errors) {
			failed(&result);
		} else if (sg_plan_build(&model, &patches, &config, &queries, &plan) && sg_generate(&config, &plan, &settings)) {
			status = 0;
		}

		sg_plan_free(&plan);
	} else if (what == MODE_PLAN) {
		sg_rules_input  input = { db, &model, &config, NULL, opts->strict, &queries, &patches };
		sg_rules_result result;
		sg_plan         plan  = {0};

		sg_rules_run(&input, &result);

		if (result.errors) {
			failed(&result);
		} else if (sg_plan_build(&model, &patches, &config, &queries, &plan)) {
			sg_buf json = {0};

			sg_plan_write_json(&plan, &json);
			status = emit(opts, &json) ? 0 : EXIT_INPUT;
			sg_buf_free(&json);
		}

		sg_plan_free(&plan);
	} else {
		sg_rules_input  input  = { db, &model, &config, NULL, opts->strict, &queries, &patches };
		sg_rules_result result;
		size_t          tables = 0;
		size_t          views  = 0;

		sg_rules_run(&input, &result);

		if (result.errors == 0) {
			size_t count = patches.patches.count;

			for (size_t i = 0; i < model.tables.count; ++i) {
				model.tables.items[i].kind == SG_VIEW ? ++views : ++tables;
			}

			if (queries.count) {
				printf("ok: %zu %s, %zu %s and %zu %s from %zu %s", tables, plural(tables, "table", "tables"),
						views, plural(views, "view", "views"), queries.count,
						plural(queries.count, "query", "queries"), count, plural(count, "patch", "patches"));
			} else {
				printf("ok: %zu %s and %zu %s from %zu %s", tables, plural(tables, "table", "tables"), views,
						plural(views, "view", "views"), count, plural(count, "patch", "patches"));
			}

			if (result.warnings) {
				printf("; %u %s", result.warnings, plural(result.warnings, "warning", "warnings"));
			}

			putchar('\n');
			status = 0;
		} else {
			failed(&result);
		}
	}

done:
	sg_queries_free(&queries);
	sg_model_free(&model);
	sqlite3_close(db);
	sg_patchset_free(&patches);
	sg_config_free(&config);
	return status;
}

/* The database --db names, read-only, with --key applied; NULL when it cannot be read. */
static sqlite3 *open_live(const options *opts) {
	sqlite3 *db = NULL;

	if (sqlite3_open_v2(opts->database, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		sg_error(opts->database, 0, 0, "cannot be opened: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}

	if (opts->key) {
#ifdef SQLITE_HAS_CODEC
		/* SQLCipher otherwise prints its own diagnostics for a wrong key, beside the one below. */
		sqlite3_exec(db, "pragma cipher_log_level = NONE", NULL, NULL, NULL);

		if (sqlite3_key(db, opts->key, (int)strlen(opts->key)) != SQLITE_OK) {
			sg_error(opts->database, 0, 0, "the key could not be applied: %s", sqlite3_errmsg(db));
			sqlite3_close(db);
			return NULL;
		}
#else
		sg_error(NULL, 0, 0, "this sqlgen was built without encryption; rebuild with make CIPHER=1");
		sqlite3_close(db);
		return NULL;
#endif
	}

	/* Opening is lazy; the first read is what finds a wrong key or a file that is no database. */
	if (sqlite3_exec(db, "select count(*) from sqlite_schema", NULL, NULL, NULL) != SQLITE_OK) {
		sg_error(opts->database, 0, 0, "cannot be read: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}

	return db;
}

static int run_report(const options *opts) {
	sqlite3   *db     = NULL;
	sg_model   model  = {0};
	sg_config  config = {0};
	int        status = EXIT_INPUT;

	if (!opts->database) {
		sg_error(NULL, 0, 0, "report needs --db <file>");
		return EXIT_USAGE;
	}

	if ((db = open_live(opts)) == NULL) {
		goto done;
	}

	/* The rule levels: from -c when given, else the defaults -- a live database needs no file. */
	if (opts->config_given ? !sg_config_load(opts->config, &config)
			: !sg_config_load_text("(defaults)", "{}", 2, &config)) {
		goto done;
	}

	if (sg_introspect(db, &model)) {
		sg_rules_input  input = { db, &model, &config, opts->database, opts->strict, NULL, NULL };
		sg_rules_result result;
		sg_buf          json  = {0};

		/* The model is printed whatever the rules find: a report writes no generated files, and the
		 * model is what explains a finding. */
		sg_rules_run(&input, &result);
		sg_model_write_json(&model, NULL, opts->database, &json);
		status = emit(opts, &json) && result.errors == 0 ? 0 : EXIT_INPUT;
		sg_buf_free(&json);

		if (result.errors) {
			failed(&result);
		}
	}

done:
	sg_model_free(&model);
	sg_config_free(&config);
	sqlite3_close(db);
	return status;
}

enum {
	DIFF_SAME      = 0,
	DIFF_DIFFERENT = 1,
	DIFF_TROUBLE   = 2   /* like diff(1): anything that kept the comparison from being made */
};

/* The patch @p text names, by precedence: build metadata does not tell patches apart. */
static long find_patch(const sg_patchset *set, const char *text) {
	sg_semver version;
	size_t    at = 0;
	long      found = -1;

	if (!sg_semver_parse(text, strlen(text), &version, &at)) {
		sg_error(NULL, 0, 0, "'%s' is not a version", text);
		return -1;
	}

	for (size_t i = 0; i < set->patches.count; ++i) {
		if (sg_semver_compare(&set->patches.items[i].version, &version) == 0) {
			found = (long)i;
		}
	}

	if (found < 0) {
		sg_error(NULL, 0, 0, "no patch has the version %s", text);
	}

	sg_semver_free(&version);
	return found;
}

static void diff_range(const sg_patchset *set, size_t from, size_t to, sg_changes *out) {
	size_t           count = to - from + 1;
	const sg_model **steps = sg_xmalloc(sizeof *steps * count);
	sg_origin       *at    = sg_xmalloc(sizeof *at * count);

	for (size_t i = 0; i < count; ++i) {
		steps[i] = &set->history.items[from + i].model;
		at[i]    = set->history.items[from + i].at;
	}

	sg_diff(steps, at, count, out);
	free(steps);
	free(at);
}

/*
 * The patch a database is at, by its _sys_patch: the newest one recorded. -1 when it has no
 * _sys_patch or no row in it; -2, reported, when it records a patch the scripts do not have.
 */
static long applied_patch(sqlite3 *db, const char *file, const sg_patchset *set) {
	sqlite3_stmt *stmt   = NULL;
	long          newest = -1;
	int           rc;

	if (sqlite3_prepare_v2(db, "select major, minor, patch, prerelease from _sys_patch", -1, &stmt, NULL)
			!= SQLITE_OK) {
		sqlite3_finalize(stmt);
		return -1;
	}

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *pre   = (const char *)sqlite3_column_text(stmt, 3);
		long        found = -1;

		for (size_t i = 0; i < set->patches.count; ++i) {
			const sg_semver *v = &set->patches.items[i].version;

			if (v->major == (uint64_t)sqlite3_column_int64(stmt, 0) && v->minor == (uint64_t)sqlite3_column_int64(stmt, 1)
					&& v->patch == (uint64_t)sqlite3_column_int64(stmt, 2) && strcmp(v->prerelease, pre ? pre : "") == 0) {
				found = (long)i;
			}
		}

		if (found < 0) {
			sg_error(file, 0, 0, "has patch %lld.%lld.%lld%s%s applied, which the scripts do not have;"
					" a newer program migrated it", sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1),
					sqlite3_column_int64(stmt, 2), pre && *pre ? "-" : "", pre ? pre : "");
			newest = -2;
			break;
		}

		newest = found > newest ? found : newest;
	}

	sqlite3_finalize(stmt);
	return newest;
}

static int run_diff(const options *opts) {
	sg_config   config  = {0};
	sg_patchset patches = {0};
	sqlite3    *db      = NULL;
	sqlite3    *live    = NULL;
	sg_model    model   = {0};
	sg_changes  changes = {0};
	int         status  = DIFF_TROUBLE;

	if (!opts->database == !opts->from || (opts->to && !opts->from)) {
		sg_error(NULL, 0, 0, "diff needs either --from <version> (and optionally --to <version>) or"
				" --db <file>");
		return DIFF_TROUBLE;
	}

	if (first_run(opts) || !sg_config_load(opts->config, &config) || !sg_patches_find(&config, &patches)) {
		goto done;
	}

	if (patches.patches.count == 0) {
		sg_error(config.path, 0, 0, "names no patch, so there is nothing to compare");
		goto done;
	}

	db = open_scratch();

	if (!db || !sg_schema_load(db, &config, &patches)) {
		goto done;
	}

	if (opts->from) {
		long from = find_patch(&patches, opts->from);
		long to   = opts->to ? find_patch(&patches, opts->to) : (long)patches.patches.count - 1;

		if (from < 0 || to < 0) {
			goto done;
		}

		if (to < from) {
			sg_error(NULL, 0, 0, "--to %s comes before --from %s", opts->to, opts->from);
			goto done;
		}

		diff_range(&patches, patches.patches.items[from].end, patches.patches.items[to].end, &changes);
	} else {
		if ((live = open_live(opts)) == NULL || !sg_introspect(live, &model)) {
			goto done;
		}

		long at   = applied_patch(live, opts->database, &patches);
		long last = (long)patches.patches.count - 1;

		if (at == -2) {
			goto done;
		}

		/* What the database lacks of later patches is pending, not drift. */
		if (at < 0) {
			fprintf(stderr, "%s: no patch recorded (_sys_patch); compared with the newest, %s\n",
					opts->database, patches.patches.items[last].version.text);
			at = last;
		} else if (at < last) {
			fprintf(stderr, "%s: at patch %s; %ld %s pending, up to %s\n", opts->database,
					patches.patches.items[at].version.text, last - at, plural((size_t)(last - at), "patch", "patches"),
					patches.patches.items[last].version.text);
		}

		const sg_model *ends[2] = { &patches.history.items[patches.patches.items[at].end].model, &model };

		sg_diff(ends, NULL, 2, &changes);
	}

	sg_buf out = {0};

	if (opts->json) {
		sg_diff_write_json(&changes, &out);
	} else {
		sg_diff_write_text(&changes, &out);
	}

	if (out.len == 0 || emit(opts, &out)) {
		status = changes.count ? DIFF_DIFFERENT : DIFF_SAME;
	}

	sg_buf_free(&out);

done:
	sg_changes_free(&changes);
	sg_model_free(&model);
	sqlite3_close(live);
	sqlite3_close(db);
	sg_patchset_free(&patches);
	sg_config_free(&config);
	return status;
}

int main(int argc, char **argv) {
	options opts;

	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		}

		if (strcmp(argv[i], "--version") == 0) {
			printf("sqlgen %s (SQLite %s)\n", SQLGEN_VERSION, sqlite3_libversion());
			return 0;
		}
	}

	if (!parse(argc, argv, &opts)) {
		usage(stderr);
		return EXIT_USAGE;
	}

	if (opts.arg_count && strcmp(opts.command, "templates") != 0) {
		sg_error(NULL, 0, 0, "unexpected argument: %s", opts.args[0]);
		usage(stderr);
		return EXIT_USAGE;
	}

	if (strcmp(opts.command, "init") == 0) {
		return run_init(&opts);
	}

	if (strcmp(opts.command, "model") == 0) {
		return run_schema(&opts, MODE_MODEL);
	}

	if (strcmp(opts.command, "check") == 0) {
		return run_schema(&opts, MODE_CHECK);
	}

	if (strcmp(opts.command, "plan") == 0) {
		if (first_run(&opts)) {
			return EXIT_INPUT;
		}

		return run_schema(&opts, MODE_PLAN);
	}

	if (strcmp(opts.command, "report") == 0) {
		return run_report(&opts);
	}

	if (strcmp(opts.command, "diff") == 0) {
		return run_diff(&opts);
	}

	if (strcmp(opts.command, "generate") == 0) {
		if (first_run(&opts)) {
			return EXIT_INPUT;
		}

		return run_schema(&opts, MODE_GENERATE);
	}

	if (strcmp(opts.command, "templates") == 0) {
		if (opts.arg_count != 2) {
			sg_error(NULL, 0, 0, "templates needs a language and a directory: sqlgen templates c my-templates");
			return EXIT_USAGE;
		}

		return sg_export_templates(opts.args[0], opts.args[1]) ? 0 : EXIT_INPUT;
	}

	sg_error(NULL, 0, 0, "unknown command: %s", opts.command);
	usage(stderr);
	return EXIT_USAGE;
}
