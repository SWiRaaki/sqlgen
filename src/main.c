#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "config.h"
#include "model.h"
#include "rules.h"
#include "schema.h"
#include "util.h"

#define SQLGEN_VERSION "0.1.0-dev"

enum {
	EXIT_INPUT = 1,   /* The input has problems; they were reported. */
	EXIT_USAGE = 2
};

typedef struct {
	const char *command;
	const char *config;
	bool        config_given;   /* -c was used, so a missing file is a mistake, not a first run */
	const char *output;
	const char *database;
	const char *key;
	bool        strict;         /* --strict: warnings are errors */
} options;

static void usage(FILE *to) {
	fputs("usage: sqlgen <command> [options]\n"
			"\n"
			"commands:\n"
			"  init      write a configuration with every default (sqlgen.json, or -c)\n"
			"  model     load the schema and print its model as JSON\n"
			"  check     load the schema and check it against the rules; writes nothing\n"
			"  report    print the model of an existing database as JSON, and check it\n"
			"            (rule levels from -c when given, else the defaults)\n"
			"  generate  write the generated sources (not implemented)\n"
			"\n"
			"options:\n"
			"  -c, --config <file>  configuration (default: sqlgen.json, written with every\n"
			"                       default when it does not exist)\n"
			"  -o, --output <file>  write to a file instead of standard output\n"
			"      --db <file>      the database to report on\n"
			"      --key <key>      its encryption key (needs a build with CIPHER=1)\n"
			"      --strict         treat rule warnings as errors\n"
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
		} else if (strcmp(arg, "--strict") == 0) {
			out->strict = true;
		} else if (arg[0] == '-') {
			sg_error(NULL, 0, 0, "unknown option or missing value: %s", arg);
			return false;
		} else if (!out->command) {
			out->command = arg;
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

static int run_schema(const options *opts, bool print) {
	sg_config   config  = {0};
	sg_patchset patches = {0};
	sg_model    model   = {0};
	sqlite3    *db      = NULL;
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

	if (print) {
		sg_buf json = {0};

		sg_model_write_json(&model, &patches, NULL, &json);
		status = emit(opts, &json) ? 0 : EXIT_INPUT;
		sg_buf_free(&json);
	} else {
		sg_rules_input  input  = { db, &model, &config, NULL, opts->strict };
		sg_rules_result result;
		size_t          tables = 0;
		size_t          views  = 0;

		sg_rules_run(&input, &result);

		if (result.errors == 0) {
			size_t count = patches.patches.count;

			for (size_t i = 0; i < model.tables.count; ++i) {
				model.tables.items[i].kind == SG_VIEW ? ++views : ++tables;
			}

			printf("ok: %zu %s and %zu %s from %zu %s", tables, plural(tables, "table", "tables"), views,
					plural(views, "view", "views"), count, plural(count, "patch", "patches"));

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
	sg_model_free(&model);
	sqlite3_close(db);
	sg_patchset_free(&patches);
	sg_config_free(&config);
	return status;
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

	if (sqlite3_open_v2(opts->database, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		sg_error(opts->database, 0, 0, "cannot be opened: %s", sqlite3_errmsg(db));
		goto done;
	}

	if (opts->key) {
#ifdef SQLITE_HAS_CODEC
		/* SQLCipher otherwise prints its own diagnostics for a wrong key, beside the one below. */
		sqlite3_exec(db, "pragma cipher_log_level = NONE", NULL, NULL, NULL);

		if (sqlite3_key(db, opts->key, (int)strlen(opts->key)) != SQLITE_OK) {
			sg_error(opts->database, 0, 0, "the key could not be applied: %s", sqlite3_errmsg(db));
			goto done;
		}
#else
		sg_error(NULL, 0, 0, "this sqlgen was built without encryption; rebuild with make CIPHER=1");
		goto done;
#endif
	}

	/* Opening is lazy; the first read is what finds a wrong key or a file that is no database. */
	if (sqlite3_exec(db, "select count(*) from sqlite_schema", NULL, NULL, NULL) != SQLITE_OK) {
		sg_error(opts->database, 0, 0, "cannot be read: %s", sqlite3_errmsg(db));
		goto done;
	}

	/* The rule levels: from -c when given, else the defaults -- a live database needs no file. */
	if (opts->config_given ? !sg_config_load(opts->config, &config)
			: !sg_config_load_text("(defaults)", "{}", 2, &config)) {
		goto done;
	}

	if (sg_introspect(db, &model)) {
		sg_rules_input  input = { db, &model, &config, opts->database, opts->strict };
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

	if (strcmp(opts.command, "init") == 0) {
		return run_init(&opts);
	}

	if (strcmp(opts.command, "model") == 0) {
		return run_schema(&opts, true);
	}

	if (strcmp(opts.command, "check") == 0) {
		return run_schema(&opts, false);
	}

	if (strcmp(opts.command, "report") == 0) {
		return run_report(&opts);
	}

	if (strcmp(opts.command, "generate") == 0) {
		/* The same first-run rule as every other command that needs a configuration. */
		if (first_run(&opts)) {
			return EXIT_INPUT;
		}

		sg_error(NULL, 0, 0, "generate is not implemented");
		return EXIT_USAGE;
	}

	sg_error(NULL, 0, 0, "unknown command: %s", opts.command);
	usage(stderr);
	return EXIT_USAGE;
}
