#include "lang.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Odin's keywords, and the built-in names a field or parameter would shadow. */
static const char *const KEYWORDS[] = {
	"asm", "auto_cast", "bit_field", "bit_set", "break", "case", "cast", "context", "continue", "defer",
	"distinct", "do", "dynamic", "else", "enum", "fallthrough", "for", "foreign", "if", "import", "in",
	"map", "matrix", "not_in", "or_else", "or_return", "or_break", "or_continue", "package", "proc",
	"return", "struct", "switch", "transmute", "typeid", "union", "using", "when", "where", "nil", "true",
	"false", "string", "cstring", "rawptr", "int", "uint", "i8", "i16", "i32", "i64", "u8", "u16",
	"u32", "u64", "f32", "f64", "bool", "byte", "rune", "any", "uintptr", "len", "cap", NULL
};

/* What a generated procedure calls its own parameters and locals, which a column's must not shadow. */
static const char *const LOCALS[] = { "ctx", "allocator", "stmt", "rc", "row", "key", "cursor", "it", NULL };

static sg_value *fail(char **error, const char *fmt, ...) SG_PRINTF(2, 3);

static sg_value *fail(char **error, const char *fmt, ...) {
	va_list args;
	char    message[256];

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);
	*error = sg_xstrdup(message);
	return NULL;
}

static bool listed(const char *const *list, const char *word) {
	for (size_t i = 0; list[i]; ++i) {
		if (strcmp(list[i], word) == 0) {
			return true;
		}
	}

	return false;
}

static void put_word(const char *word, void *user) {
	sg_buf *out = user;

	if (out->len) {
		sg_buf_putc(out, '_');
	}

	sg_buf_puts(out, word);
}

static void put_ada(const char *word, void *user) {
	sg_buf *out = user;

	if (out->len) {
		sg_buf_putc(out, '_');
	}

	sg_buf_putc(out, (char)(word[0] >= 'a' && word[0] <= 'z' ? word[0] - 'a' + 'A' : word[0]));
	sg_buf_puts(out, word + 1);
}

/* A text or a list of words, joined by @p put. */
static bool words(const sg_value *v, void (*put)(const char *, void *), sg_buf *out) {
	if (v && v->kind == SG_V_TEXT) {
		sg_split_words(v->text, put, out);
		return true;
	}

	if (!v || v->kind != SG_V_LIST) {
		return false;
	}

	for (size_t i = 0; i < v->items.count; ++i) {
		if (v->items.items[i]->kind != SG_V_TEXT) {
			return false;
		}

		sg_split_words(v->items.items[i]->text, put, out);
	}

	return true;
}

/* Snake case, `_` before a leading digit, `_` after a word Odin has taken (or @p locals, too). */
static sg_value *name(const sg_value *in, size_t count, bool locals, char **error) {
	sg_buf out = {0};

	if (count || !words(in, put_word, &out)) {
		sg_buf_free(&out);
		return fail(error, "%s takes a text or a list of words, and no arguments", locals ? "odinparam" : "odinname");
	}

	if (!out.data) {
		return sg_value_text("_");
	}

	if (out.data[0] >= '0' && out.data[0] <= '9') {
		char *named = sg_format("_%s", out.data);

		sg_buf_free(&out);
		return sg_value_text_owned(named);
	}

	if (listed(KEYWORDS, out.data) || (locals && listed(LOCALS, out.data))) {
		sg_buf_putc(&out, '_');
	}

	return sg_value_text_owned(out.data);
}

static sg_value *odinname(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;
	return name(in, count, false, error);
}

static sg_value *odinparam(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;
	return name(in, count, true, error);
}

static const char *text_member(const sg_value *map, const char *key) {
	const sg_value *v = sg_map_get(map, key);

	return v && v->kind == SG_V_TEXT ? v->text : NULL;
}

static const char *storage_of(const sg_lang_context *ctx, const sg_value *column) {
	const char *affinity = text_member(column, "affinity");

	if (text_member(column, "id_of") && sg_value_truthy(sg_map_get(ctx->root, "typed_ids"))) {
		return "id";
	}

	if (!affinity || strcmp(affinity, "numeric") == 0 || strcmp(affinity, "any") == 0) {
		return "value";
	}

	return strcmp(affinity, "integer") == 0 ? "int" : affinity;
}

/* <Table>_<Key_Column>, the distinct type of a table's typed id. */
static char *id_type(const sg_lang_context *ctx, const char *table_name, char **error) {
	const sg_value *tables = sg_map_get(ctx->root, "tables");

	for (size_t t = 0; tables && t < tables->items.count; ++t) {
		const sg_value *table   = tables->items.items[t];
		const char     *key     = text_member(table, "id_column");
		const sg_value *columns = sg_map_get(table, "columns");

		if (!text_member(table, "name") || strcmp(text_member(table, "name"), table_name) != 0) {
			continue;
		}

		for (size_t i = 0; key && columns && i < columns->items.count; ++i) {
			const sg_value *c = columns->items.items[i];

			if (strcmp(text_member(c, "name"), key) == 0) {
				sg_buf out = {0};

				/* put_ada separates the column's words from the table's itself. */
				words(sg_map_get(table, "words"), put_ada, &out);
				words(sg_map_get(c, "words"), put_ada, &out);
				return out.data;
			}
		}
	}

	*error = sg_format("table %s has no typed id", table_name);
	return NULL;
}

static sg_value *storage(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;

	if (count || in->kind != SG_V_MAP) {
		return fail(error, "storage takes a column and no arguments");
	}

	return sg_value_text(storage_of(user, in));
}

static sg_value *odintype(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	static const char *const TYPES[][2] = {
		{ "int", "i64" }, { "real", "f64" }, { "text", "string" }, { "blob", "[]u8" }, { "value", "Value" }
	};
	const char *kind;

	(void)args;

	if (count || in->kind != SG_V_MAP) {
		return fail(error, "odintype takes a column and no arguments");
	}

	kind = storage_of(user, in);

	if (strcmp(kind, "id") == 0) {
		char *type = id_type(user, text_member(in, "id_of"), error);

		return type ? sg_value_text_owned(type) : NULL;
	}

	for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; ++i) {
		if (strcmp(kind, TYPES[i][0]) == 0) {
			return sg_value_text(TYPES[i][1]);
		}
	}

	return fail(error, "no Odin type for the storage %s", kind);
}

static sg_value *idtype(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;

	if (count || in->kind != SG_V_MAP || !text_member(in, "name")) {
		return fail(error, "idtype takes a table and no arguments");
	}

	char *type = id_type(user, text_member(in, "name"), error);

	return type ? sg_value_text_owned(type) : NULL;
}

/* One byte of an Odin string literal; anything but printable ASCII as \xHH, so every text is valid. */
static void escape(sg_buf *out, unsigned char c) {
	switch (c) {
	case '\\': sg_buf_puts(out, "\\\\"); return;
	case '"':  sg_buf_puts(out, "\\\""); return;
	case '\n': sg_buf_puts(out, "\\n"); return;
	case '\t': sg_buf_puts(out, "\\t"); return;
	case '\r': sg_buf_puts(out, "\\r"); return;
	default:
		if (c < 0x20 || c >= 0x7f) {
			sg_buf_printf(out, "\\x%02x", c);
		} else {
			sg_buf_putc(out, (char)c);
		}
	}
}

static sg_value *odinstring(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	sg_buf out = {0};

	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		return fail(error, "odinstring takes a text and no arguments");
	}

	sg_buf_putc(&out, '"');

	for (const char *c = in->text; *c; ++c) {
		escape(&out, (unsigned char)*c);
	}

	sg_buf_putc(&out, '"');
	return sg_value_text_owned(out.data);
}

/* A text as a list of literals, one per line, each line break kept at the end of its literal; joined
 * with `+`, they are one constant. */
static sg_value *odinstrings(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		return fail(error, "odinstrings takes a text and no arguments");
	}

	sg_value   *list = sg_value_list();
	const char *s    = in->text;

	do {
		size_t n    = strcspn(s, "\n");
		bool   nl   = s[n] == '\n';
		sg_buf line = {0};

		sg_buf_putc(&line, '"');

		for (size_t i = 0; i < n + nl; ++i) {
			escape(&line, (unsigned char)s[i]);
		}

		sg_buf_putc(&line, '"');
		sg_list_push(list, sg_value_text_owned(line.data));
		s += n + nl;
	} while (*s);

	return list;
}

size_t sg_lang_odin_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context) {
	const sg_filter filters[] = {
		{ "odinname",    odinname,    context },
		{ "odinparam",   odinparam,   context },
		{ "storage",     storage,     context },
		{ "odintype",    odintype,    context },
		{ "idtype",      idtype,      context },
		{ "odinstring",  odinstring,  context },
		{ "odinstrings", odinstrings, context },
	};
	size_t n = sizeof filters / sizeof filters[0];

	memcpy(out, filters, sizeof filters);
	return n;
}

/* ---- the check ----------------------------------------------------------------------------- */

typedef struct {
	char       *name;
	const char *owner;   //!< "the table x", "the query y.sql", "the runtime".
	const char *file;    //!< A query's file, where its clash is reported; NULL otherwise.
} taken;

typedef SG_VEC(taken) takens;

static char *joined(const sg_words *w, void (*put)(const char *, void *), const char *suffix) {
	sg_buf out = {0};

	for (size_t i = 0; i < w->count; ++i) {
		sg_split_words(w->items[i], put, &out);
	}

	sg_buf_puts(&out, suffix);
	return out.data ? out.data : sg_xstrdup(suffix);
}

static void take(takens *list, char *name, const char *owner) {
	taken *t = SG_PUSH(*list);

	t->name  = name;
	t->owner = owner;
	t->file  = NULL;
}

/*
 * One package holds everything, so every name a table, a query or the runtime defines must be its
 * own: procedures in snake case, types in Ada case.
 */
bool sg_lang_odin_check(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan) {
	static const char *const RUNTIME[] = {
		"context_make", "context_destroy", "cursor_next", "cursor_close", "value_destroy", "migrate",
		"systables", "patches", "schema_hash", "prepared", "cursor_open", "finish", "run", "check_columns",
		"id_of", "read_id", "read_bytes", "read_text", "read_blob", "read_value", "bind_text", "bind_blob",
		"bind_value", "bind_id", "EMPTY", "Context", "Cursor", "Result", "Value", "Script", "Patch",
		"Sqlite3", "Sqlite3_Stmt", "Statement", NULL
	};
	/* Each table and query has a file named after it, beside the runtime's. */
	static const char *const FILES[] = { "sqlite3", "scripts", "migrate", NULL };
	static const char *const TABLE_PROCS[] = { "_read", "_row_destroy", NULL };
	takens  names  = {0};
	SG_VEC(char *) owners = {0};
	bool    ok     = true;

	(void)config;
	(void)index;

	if (listed(KEYWORDS, target->prefix)) {
		sg_error(config->path, 0, 0, "$.targets[%zu].prefix: '%s' is a word Odin has taken; a package needs"
				" another name", index, target->prefix);
		ok = false;
	}

	for (size_t i = 0; RUNTIME[i]; ++i) {
		take(&names, sg_xstrdup(RUNTIME[i]), "the runtime");
	}

	for (size_t i = 0; FILES[i]; ++i) {
		take(&names, sg_format("%s.odin", FILES[i]), "the runtime");
	}

	take(&names, sg_format("%s.odin", target->prefix), "the runtime");

	size_t from = names.count;

	for (size_t t = 0; t < plan->tables.count; ++t) {
		const sg_plan_table *table = &plan->tables.items[t];
		char                *owner = sg_format("the table %s", table->table->name);

		*SG_PUSH(owners) = owner;
		take(&names, joined(&table->words, put_word, ".odin"), owner);
		take(&names, joined(&table->words, put_ada, "_Row"), owner);

		for (size_t i = 0; TABLE_PROCS[i]; ++i) {
			take(&names, joined(&table->words, put_word, TABLE_PROCS[i]), owner);
		}

		for (size_t s = 0; s < table->statements.count; ++s) {
			char *stmt = joined(&table->statements.items[s].words, put_word, "");
			char *base = joined(&table->words, put_word, "_");
			char *full = sg_format("%s%s", base, stmt);

			take(&names, full, owner);
			free(stmt);
			free(base);
		}

		if (table->id_column >= 0) {
			char *base = joined(&table->words, put_ada, "_");
			char *col  = joined(&table->columns.items[table->id_column].words, put_ada, "");

			take(&names, sg_format("%s%s", base, col), owner);
			free(base);
			free(col);
		}
	}

	for (size_t q = 0; plan->queries && q < plan->queries->count; ++q) {
		const sg_query *query = &plan->queries->items[q];
		char           *owner = sg_format("the query %s", query->path);

		*SG_PUSH(owners) = owner;
		take(&names, joined(&query->words, put_word, ".odin"), owner);
		take(&names, joined(&query->words, put_word, ""), owner);

		if (!query->row_table) {
			take(&names, joined(&query->words, put_ada, "_Row"), owner);
			take(&names, joined(&query->words, put_word, "_read"), owner);
			take(&names, joined(&query->words, put_word, "_row_destroy"), owner);
		}

		for (size_t i = names.count; i > 0 && names.items[i - 1].owner == owner; --i) {
			names.items[i - 1].file = query->path;
		}
	}

	/* Each table's or query's first clash says enough. Two tables can clash too: SQLite tells
	 * UserTag from user_tag, but their words are the same. */
	for (size_t i = from; i < names.count; ++i) {
		for (size_t j = 0; j < i; ++j) {
			if (names.items[j].owner != names.items[i].owner && strcmp(names.items[i].name, names.items[j].name) == 0) {
				sg_error(names.items[i].file, 0, 0, "%s would define %s in the Odin package %s, which %s already defines",
						names.items[i].owner, names.items[i].name, target->prefix, names.items[j].owner);
				ok = false;

				while (i + 1 < names.count && names.items[i + 1].owner == names.items[i].owner) {
					++i;
				}

				break;
			}
		}
	}

	for (size_t i = 0; i < names.count; ++i) {
		free(names.items[i].name);
	}

	for (size_t i = 0; i < owners.count; ++i) {
		free(owners.items[i]);
	}

	free(names.items);
	free(owners.items);
	return ok;
}
