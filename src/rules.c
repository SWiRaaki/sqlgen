#include "rules.h"

#include "diff.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	const sg_rules_input *in;
	sg_rules_result      *out;
	const sg_rule_config *rule;   /* The rule being run. */
} run;

static void finding(run *r, const sg_table *table, const char *fmt, ...) SG_PRINTF(3, 4);

static void finding(run *r, const sg_table *table, const char *fmt, ...) {
	va_list args;
	char    message[512];

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);

	const char *file  = table && table->created.script ? table->created.script : r->in->source;
	unsigned    line  = table && table->created.script ? table->created.line : 0;
	unsigned    col   = table && table->created.script ? table->created.col : 0;
	bool        error = r->rule->level == SG_LEVEL_ERROR || r->in->strict;

	if (error) {
		sg_error(file, line, col, "%s [%s]", message, r->rule->name);
		++r->out->errors;
	} else {
		sg_warning(file, line, col, "%s [%s]", message, r->rule->name);
		++r->out->warnings;
	}
}

/* "a, b" */
static void join(sg_buf *out, const sg_names *names) {
	for (size_t i = 0; i < names->count; ++i) {
		sg_buf_puts(out, i ? ", " : "");
		sg_buf_puts(out, names->items[i]);
	}
}

/* "table(a, b)" in a fresh allocation. */
static char *columns_of(const char *table, const sg_names *names) {
	sg_buf out = {0};

	sg_buf_puts(&out, table);
	sg_buf_putc(&out, '(');
	join(&out, names);
	sg_buf_putc(&out, ')');
	return out.data;
}

/* Whether @p a and @p b hold the same names, in any order, compared as SQLite compares names. */
static bool same_names(const sg_names *a, const sg_names *b) {
	if (a->count != b->count) {
		return false;
	}

	for (size_t i = 0; i < a->count; ++i) {
		bool found = false;

		for (size_t j = 0; j < b->count && !found; ++j) {
			found = sg_strcasecmp(a->items[i], b->items[j]) == 0;
		}

		if (!found) {
			return false;
		}
	}

	return true;
}

static bool is_unique_key_of(const sg_table *parent, const sg_names *columns) {
	for (size_t i = 0; i < parent->unique_keys.count; ++i) {
		if (same_names(&parent->unique_keys.items[i].columns, columns)) {
			return true;
		}
	}

	return false;
}

/* Whether SQLite would report "foreign key mismatch" for @p table; its message when it does. */
static char *mismatch(sqlite3 *db, const sg_table *table) {
	char         *sql  = sqlite3_mprintf("pragma foreign_key_check(%Q)", table->name);
	sqlite3_stmt *stmt = NULL;
	char         *seen = NULL;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK
			&& strstr(sqlite3_errmsg(db), "foreign key mismatch")) {
		seen = sg_xstrdup(sqlite3_errmsg(db));
	}

	sqlite3_finalize(stmt);
	sqlite3_free(sql);
	return seen;
}

static void fk_target(run *r, const sg_table *table) {
	char *sqlite_says = mismatch(r->in->db, table);
	bool  named       = false;

	for (size_t i = 0; i < table->foreign_keys.count; ++i) {
		const sg_foreign_key *fk     = &table->foreign_keys.items[i];
		const sg_table       *parent = sg_model_table(r->in->model, fk->parent);
		char                 *child  = columns_of(table->name, &fk->columns);

		if (!parent) {
			finding(r, table, "%s references %s, which does not exist", child, fk->parent);
		} else if (sqlite_says && fk->resolved.count == 0) {
			finding(r, table, "%s references the primary key of %s, which has none", child, parent->name);
			named = true;
		} else if (sqlite_says && !is_unique_key_of(parent, &fk->resolved)) {
			char *target = columns_of(parent->name, &fk->resolved);

			finding(r, table, "%s references %s, which is neither its primary key nor a unique key;"
					" SQLite refuses writes to %s with \"foreign key mismatch\"", child, target, table->name);
			free(target);
			named = true;
		}

		free(child);
	}

	/* SQLite's verdict stands even where the model cannot say which key it means. */
	if (sqlite_says && !named) {
		finding(r, table, "%s", sqlite_says);
	}

	free(sqlite_says);
}

static void fk_indexed(run *r, const sg_table *table) {
	for (size_t i = 0; i < table->foreign_keys.count; ++i) {
		const sg_foreign_key *fk = &table->foreign_keys.items[i];

		if (fk->indexed) {
			continue;
		}

		sg_buf columns = {0};

		join(&columns, &fk->columns);
		finding(r, table, "%s(%s) references %s, but no index of %s starts with (%s); deleting from"
				" %s scans %s", table->name, columns.data, fk->parent, table->name, columns.data,
				fk->parent, table->name);
		sg_buf_free(&columns);
	}
}

static void nullable_unique(run *r, const sg_table *table) {
	for (size_t i = 0; i < table->unique_keys.count; ++i) {
		const sg_unique_key *key = &table->unique_keys.items[i];

		if (key->total) {
			continue;
		}

		sg_names nullable = {0};

		for (size_t c = 0; c < key->columns.count; ++c) {
			const sg_column *column = sg_table_column(table, key->columns.items[c]);

			if (column && !column->notnull) {
				*SG_PUSH(nullable) = key->columns.items[c];
			}
		}

		char  *whole = columns_of(table->name, &key->columns);
		sg_buf which = {0};

		join(&which, &nullable);
		finding(r, table, "unique key %s holds nullable %s; SQLite treats NULLs as distinct, so any"
				" number of rows may share it", whole, which.data);
		sg_buf_free(&which);
		free(whole);
		free(nullable.items);
	}
}

static void strict(run *r, const sg_table *table) {
	if (!table->strict) {
		finding(r, table, "%s is not STRICT", table->name);
	}
}

static void pk_shape(run *r, const sg_table *table) {
	if (table->junction || table->rowid_alias >= 0) {
		return;
	}

	sg_names key = {0};

	for (int position = 1; position <= (int)table->columns.count; ++position) {
		for (size_t c = 0; c < table->columns.count; ++c) {
			if (table->columns.items[c].pk == position) {
				*SG_PUSH(key) = table->columns.items[c].name;
			}
		}
	}

	if (table->without_rowid) {
		finding(r, table, "%s is WITHOUT ROWID; it needs an INTEGER PRIMARY KEY that is its rowid",
				table->name);
	} else if (key.count == 0) {
		finding(r, table, "%s has no primary key; it needs an INTEGER PRIMARY KEY that is its rowid",
				table->name);
	} else if (key.count > 1) {
		char *whole = columns_of(table->name, &key);

		finding(r, table, "the primary key %s has %zu columns; it needs to be one INTEGER PRIMARY"
				" KEY that is the rowid", whole, key.count);
		free(whole);
	} else {
		const sg_column *column = sg_table_column(table, key.items[0]);

		/* A single primary-key column declared exactly INTEGER in a rowid table is the rowid unless
		 * it is descending, so DESC is the only thing that can have kept it from being one. */
		if (sg_strcasecmp(column->declared_type, "INTEGER") == 0) {
			finding(r, table, "%s.%s is INTEGER PRIMARY KEY DESC, which SQLite does not make the"
					" rowid; drop DESC", table->name, column->name);
		} else {
			finding(r, table, "%s.%s is the primary key but declared '%s'; only INTEGER PRIMARY KEY"
					" makes a column the rowid", table->name, column->name, column->declared_type);
		}
	}

	free(key.items);
}

static void pk_naming(run *r, const sg_table *table) {
	if (table->rowid_alias < 0) {
		return;
	}

	const sg_column *key    = &table->columns.items[table->rowid_alias];
	const char      *wanted = sg_rule_param(r->rule, key->autoincrement ? "autoincrement" : "plain");

	if (sg_strcasecmp(key->name, wanted) != 0) {
		finding(r, table, "%s.%s is the primary key %s AUTOINCREMENT, so it should be named %s",
				table->name, key->name, key->autoincrement ? "with" : "without", wanted);
	}
}

static void created_at(run *r, const sg_table *table) {
	const char *column = sg_rule_param(r->rule, "column");

	if (!table->junction && !sg_table_column(table, column)) {
		finding(r, table, "%s has no %s column", table->name, column);
	}
}

static void updated_at_needs_created_at(run *r, const sg_table *table) {
	const char *updated = sg_rule_param(r->rule, "updated");
	const char *created = sg_rule_param(r->rule, "created");

	if (sg_table_column(table, updated) && !sg_table_column(table, created)) {
		finding(r, table, "%s has %s but no %s", table->name, updated, created);
	}
}

static void stamp_type(run *r, const sg_table *table) {
	const char *pattern = sg_rule_param(r->rule, "pattern");

	for (size_t i = 0; i < table->columns.count; ++i) {
		const sg_column *column = &table->columns.items[i];

		if (fnmatch(pattern, column->name, 0) == 0 && column->affinity != SG_AFF_INTEGER) {
			finding(r, table, "%s.%s matches '%s' but is declared '%s', which is not an integer type",
					table->name, column->name, pattern, column->declared_type);
		}
	}
}

static void prefix_origin(run *r, const sg_table *table) {
	const char *prefix   = sg_rule_param(r->rule, "prefix");
	bool        prefixed = strncmp(table->name, prefix, strlen(prefix)) == 0;

	if (!table->created.script) {
		return;
	}

	if (prefixed && !table->created.system) {
		finding(r, table, "%s starts with %s, which is reserved for tables the system-tables script"
				" creates", table->name, prefix);
	} else if (!prefixed && table->created.system) {
		finding(r, table, "%s is created by the system-tables script but does not start with %s",
				table->name, prefix);
	}
}

typedef void (*rule_fn)(run *r, const sg_table *table);

/* Rules that look at a table's own shape apply to tables only, not to views or virtual tables. */
static const struct {
	const char *name;
	rule_fn     check;
	bool        shape;
} RULES[] = {
	{ "fk_target",                   fk_target,                   false },
	{ "fk_indexed",                  fk_indexed,                  false },
	{ "nullable_unique",             nullable_unique,             false },
	{ "strict",                      strict,                      true },
	{ "pk_shape",                    pk_shape,                    true },
	{ "pk_naming",                   pk_naming,                   true },
	{ "created_at",                  created_at,                  true },
	{ "updated_at_needs_created_at", updated_at_needs_created_at, true },
	{ "stamp_type",                  stamp_type,                  true },
	{ "prefix_origin",               prefix_origin,               false },
	{ NULL, NULL, false }
};

static void unknown_tables(const sg_rules_input *in, sg_rules_result *out) {
	for (size_t i = 0; i < in->config->tables.count; ++i) {
		const char *name = in->config->tables.items[i].name;

		if (strcmp(name, "*") != 0 && !sg_model_table(in->model, name)) {
			sg_error(in->config->path, 0, 0, "$.tables.%s names no table in the schema", name);
			++out->errors;
		}
	}
}

/* A full table scan in a query, as `explain query plan` reports it. */
static void query_scan(run *r, const sg_queries *queries) {
	for (size_t i = 0; queries && i < queries->count; ++i) {
		const sg_query *q = &queries->items[i];

		for (size_t s = 0; !q->allow_scan && s < q->scans.count; ++s) {
			char    message[512];
			bool    error = r->rule->level == SG_LEVEL_ERROR || r->in->strict;

			snprintf(message, sizeof message, "%s scans %s in full; an index would let it seek, or"
					" -- @allow scan says it is meant [query_scan]", q->name, q->scans.items[s]);

			if (error) {
				sg_error(q->path, q->line, q->col, "%s", message);
				++r->out->errors;
			} else {
				sg_warning(q->path, q->line, q->col, "%s", message);
				++r->out->warnings;
			}
		}
	}
}

/* A finding of a migration rule, at the statement that made the change, else at the patch. */
static void migration_finding(run *r, const char *rule, const sg_patch *patch, const sg_change *c,
		const char *fmt, ...) SG_PRINTF(5, 6);

static void migration_finding(run *r, const char *rule, const sg_patch *patch, const sg_change *c,
		const char *fmt, ...) {
	const sg_rule_config *config = sg_config_rule(r->in->config, rule);

	if (!config || config->level == SG_LEVEL_OFF) {
		return;
	}

	va_list args;
	char    message[512];

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);

	const char *file  = c->located ? c->at.script : patch->manifest;
	unsigned    line  = c->located ? c->at.line : 0;
	unsigned    col   = c->located ? c->at.col : 0;

	if (config->level == SG_LEVEL_ERROR || r->in->strict) {
		sg_error(file, line, col, "patch %s %s [%s]", patch->version.text, message, rule);
		++r->out->errors;
	} else {
		sg_warning(file, line, col, "patch %s %s [%s]", patch->version.text, message, rule);
		++r->out->warnings;
	}
}

/* A declared type in lowercase, like the rest of a message; into @p out, which holds 64 bytes. */
static const char *declared(const sg_column *c, char *out) {
	const char *type = c->declared_type[0] ? c->declared_type : "no type";
	size_t      i    = 0;

	for (; type[i] && i < 63; ++i) {
		out[i] = (char)tolower((unsigned char)type[i]);
	}

	out[i] = '\0';
	return out;
}

static void migration_change(run *r, const sg_patch *patch, const sg_change *c) {
	if (c->thing == SG_THING_TABLE && c->kind == SG_CHANGE_REMOVE && c->old_table->kind == SG_TABLE) {
		migration_finding(r, "drop_table", patch, c, "drops table %s; its rows are lost", c->table);
	} else if (c->thing == SG_THING_COLUMN && c->kind == SG_CHANGE_REMOVE) {
		migration_finding(r, "drop_column", patch, c, "drops column %s.%s; its values are lost", c->table,
				c->name);
	} else if (c->thing == SG_THING_COLUMN && c->kind == SG_CHANGE_ALTER) {
		char was[64];
		char is[64];

		if (sg_strcasecmp(c->old_column->declared_type, c->new_column->declared_type) != 0) {
			migration_finding(r, "column_type", patch, c, "changes %s.%s from %s to %s; copied values are"
					" converted where they can be and kept as they are where not (a STRICT table refuses them)",
					c->table, c->name, declared(c->old_column, was), declared(c->new_column, is));
		}

		if (!c->old_column->notnull_declared && c->new_column->notnull_declared) {
			migration_finding(r, "not_null_added", patch, c, "makes %s.%s NOT NULL; the patch fails on a row"
					" that holds NULL there", c->table, c->name);
		}
	} else if (c->kind == SG_CHANGE_ADD && c->old_table && (c->thing == SG_THING_UNIQUE
			|| (c->thing == SG_THING_INDEX && strncmp(c->after, "unique ", 7) == 0))) {
		migration_finding(r, "unique_added", patch, c, "adds %s %s to %s, which existed; the patch fails on"
				" rows that repeat it", c->thing == SG_THING_INDEX ? "the unique index" : "the key",
				c->thing == SG_THING_INDEX ? c->name : c->after, c->table);
	} else if (c->kind == SG_CHANGE_RENAME && c->thing == SG_THING_TABLE) {
		migration_finding(r, "rename", patch, c, "renames table %s to %s; the generated names change with it",
				c->old_name, c->table);
	} else if (c->kind == SG_CHANGE_RENAME && c->thing == SG_THING_COLUMN) {
		migration_finding(r, "rename", patch, c, "renames column %s.%s to %s; the generated names change"
				" with it", c->table, c->old_name, c->name);
	}
}

/* Each patch against the schema before it, through the snapshots it took. */
static void migrations(run *r, const sg_patchset *set) {
	for (size_t p = 0; set && p < set->patches.count; ++p) {
		const sg_patch *patch = &set->patches.items[p];
		size_t          from  = p ? set->patches.items[p - 1].end : 0;
		size_t          count = patch->end - from + 1;

		if (count < 2) {
			continue;
		}

		const sg_model **steps = sg_xmalloc(sizeof *steps * count);
		sg_origin       *at    = sg_xmalloc(sizeof *at * count);
		sg_changes       changes;

		for (size_t i = 0; i < count; ++i) {
			steps[i] = &set->history.items[from + i].model;
			at[i]    = set->history.items[from + i].at;
		}

		sg_diff(steps, at, count, &changes);

		for (size_t i = 0; i < changes.count; ++i) {
			migration_change(r, patch, &changes.items[i]);
		}

		sg_changes_free(&changes);
		free(steps);
		free(at);
	}
}

void sg_rules_run(const sg_rules_input *in, sg_rules_result *out) {
	run r = { in, out, NULL };

	memset(out, 0, sizeof *out);

	for (size_t t = 0; t < in->model->tables.count; ++t) {
		const sg_table *table = &in->model->tables.items[t];

		for (size_t i = 0; RULES[i].name; ++i) {
			r.rule = sg_config_rule(in->config, RULES[i].name);

			if (!r.rule || r.rule->level == SG_LEVEL_OFF) {
				continue;
			}

			if (table->kind != SG_TABLE && (RULES[i].shape || table->kind == SG_VIEW)) {
				continue;
			}

			RULES[i].check(&r, table);
		}
	}

	r.rule = sg_config_rule(in->config, "query_scan");

	if (r.rule && r.rule->level != SG_LEVEL_OFF) {
		query_scan(&r, in->queries);
	}

	migrations(&r, in->patches);
	unknown_tables(in, out);
}
