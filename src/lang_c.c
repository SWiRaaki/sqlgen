#include "lang.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const KEYWORDS[] = {
	"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
	"extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
	"short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
	"volatile", "while", "_Bool", "_Complex", "_Imaginary", "_Alignas", "_Alignof", "_Atomic",
	"_Generic", "_Noreturn", "_Static_assert", "_Thread_local", "bool", "true", "false", "NULL", NULL
};

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

static void snake_word(const char *word, void *user) {
	sg_buf *out = user;

	if (out->len) {
		sg_buf_putc(out, '_');
	}

	sg_buf_puts(out, word);
}

/* A text, or a list of words, in snake case. */
static bool snake(const sg_value *v, sg_buf *out) {
	if (v->kind == SG_V_TEXT) {
		sg_split_words(v->text, snake_word, out);
		return true;
	}

	if (v->kind != SG_V_LIST) {
		return false;
	}

	for (size_t i = 0; i < v->items.count; ++i) {
		if (v->items.items[i]->kind != SG_V_TEXT) {
			return false;
		}

		sg_split_words(v->items.items[i]->text, snake_word, out);
	}

	return true;
}

static sg_value *cname(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	sg_buf out = {0};

	(void)args;
	(void)user;

	if (count || !snake(in, &out)) {
		sg_buf_free(&out);
		return fail(error, "cname takes a text or a list of words, and no arguments");
	}

	if (!out.data) {
		return sg_value_text("_");
	}

	if (out.data[0] >= '0' && out.data[0] <= '9') {
		char *named = sg_format("_%s", out.data);

		sg_buf_free(&out);
		return sg_value_text_owned(named);
	}

	for (size_t i = 0; KEYWORDS[i]; ++i) {
		if (strcmp(out.data, KEYWORDS[i]) == 0) {
			sg_buf_putc(&out, '_');
		}
	}

	return sg_value_text_owned(out.data);
}

static const char *text_member(const sg_value *map, const char *key) {
	const sg_value *v = sg_map_get(map, key);

	return v && v->kind == SG_V_TEXT ? v->text : NULL;
}

static bool flag_member(const sg_value *map, const char *key) {
	return sg_value_truthy(sg_map_get(map, key));
}

static const sg_value *table_named(const sg_value *root, const char *name) {
	const sg_value *tables = sg_map_get(root, "tables");

	for (size_t i = 0; tables && i < tables->items.count; ++i) {
		const char *have = text_member(tables->items.items[i], "name");

		if (have && strcmp(have, name) == 0) {
			return tables->items.items[i];
		}
	}

	return NULL;
}

static const char *storage_of(const sg_lang_context *ctx, const sg_value *column) {
	const char *affinity = text_member(column, "affinity");

	if (text_member(column, "id_of") && flag_member(ctx->root, "typed_ids")) {
		return "id";
	}

	if (!affinity || strcmp(affinity, "numeric") == 0 || strcmp(affinity, "any") == 0) {
		return "value";
	}

	return strcmp(affinity, "integer") == 0 ? "int" : affinity;
}

/* <prefix>_<table>_<key column>, for the table @p table. */
static char *id_type(const sg_lang_context *ctx, const sg_value *table, char **error) {
	const char     *key     = text_member(table, "id_column");
	const sg_value *columns = sg_map_get(table, "columns");
	sg_buf          out     = {0};

	if (!key || !columns) {
		*error = sg_format("table %s has no typed id", text_member(table, "name"));
		return NULL;
	}

	for (size_t i = 0; i < columns->items.count; ++i) {
		const sg_value *c = columns->items.items[i];

		if (strcmp(text_member(c, "name"), key) == 0) {
			sg_buf table_part  = {0};
			sg_buf column_part = {0};

			snake(sg_map_get(table, "words"), &table_part);
			snake(sg_map_get(c, "words"), &column_part);
			sg_buf_printf(&out, "%s_%s_%s", ctx->target->prefix, table_part.data ? table_part.data : "",
					column_part.data ? column_part.data : "");
			sg_buf_free(&table_part);
			sg_buf_free(&column_part);
		}
	}

	return out.data;
}

static sg_value *storage(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;

	if (count || in->kind != SG_V_MAP) {
		return fail(error, "storage takes a column and no arguments");
	}

	return sg_value_text(storage_of(user, in));
}

static sg_value *ctype(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	const sg_lang_context *ctx = user;

	(void)args;

	if (count || in->kind != SG_V_MAP) {
		return fail(error, "ctype takes a column and no arguments");
	}

	const char *kind = storage_of(ctx, in);

	if (strcmp(kind, "id") == 0) {
		const sg_value *owner = table_named(ctx->root, text_member(in, "id_of"));

		if (!owner) {
			return fail(error, "no table %s for the id of %s", text_member(in, "id_of"), text_member(in, "name"));
		}

		char *type = id_type(ctx, owner, error);

		return type ? sg_value_text_owned(type) : NULL;
	}

	if (flag_member(in, "custom")) {
		const char *logical = text_member(in, "type");

		for (size_t i = 0; i < ctx->target->types.count; ++i) {
			if (strcmp(ctx->target->types.items[i].key, logical) == 0) {
				return sg_value_text(ctx->target->types.items[i].value);
			}
		}

		return fail(error, "the target maps no C type for the logical type %s", logical);
	}

	if (strcmp(kind, "int") == 0) {
		return sg_value_text("int64_t");
	}

	if (strcmp(kind, "real") == 0) {
		return sg_value_text("double");
	}

	return sg_value_text_owned(sg_format("%s_%s", ctx->target->prefix, kind));
}

static sg_value *idtype(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;

	if (count || in->kind != SG_V_MAP) {
		return fail(error, "idtype takes a table and no arguments");
	}

	char *type = id_type(user, in, error);

	return type ? sg_value_text_owned(type) : NULL;
}

/* One character of a C string literal. A '?' after a '?' is escaped, so no trigraph forms. */
static void escape(sg_buf *out, unsigned char c, unsigned char previous) {
	switch (c) {
	case '\\': sg_buf_puts(out, "\\\\"); return;
	case '"':  sg_buf_puts(out, "\\\""); return;
	case '\n': sg_buf_puts(out, "\\n"); return;
	case '\t': sg_buf_puts(out, "\\t"); return;
	case '\r': sg_buf_puts(out, "\\r"); return;
	case '?':  sg_buf_puts(out, previous == '?' ? "\\?" : "?"); return;
	default:
		if (c < 0x20 || c == 0x7f) {
			sg_buf_printf(out, "\\%03o", c);
		} else {
			sg_buf_putc(out, (char)c);
		}
	}
}

static sg_value *cstring(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	sg_buf out = {0};

	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		return fail(error, "cstring takes a text and no arguments");
	}

	sg_buf_putc(&out, '"');

	for (size_t i = 0; in->text[i]; ++i) {
		escape(&out, (unsigned char)in->text[i], i ? (unsigned char)in->text[i - 1] : 0);
	}

	sg_buf_putc(&out, '"');
	return sg_value_text_owned(out.data);
}

static sg_value *cstrings(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	sg_value *list = sg_value_list();
	sg_buf    line = {0};

	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		sg_value_free(list);
		return fail(error, "cstrings takes a text and no arguments");
	}

	for (size_t i = 0; ; ++i) {
		unsigned char c = (unsigned char)in->text[i];

		if (c == '\0' && (line.len || list->items.count == 0)) {
			sg_buf_putc(&line, '"');
			sg_list_push(list, sg_value_text_owned(sg_format("\"%s", line.data)));
			sg_buf_free(&line);
		}

		if (c == '\0') {
			break;
		}

		escape(&line, c, i ? (unsigned char)in->text[i - 1] : 0);

		if (c == '\n') {
			sg_buf_putc(&line, '"');
			sg_list_push(list, sg_value_text_owned(sg_format("\"%s", line.data)));
			sg_buf_free(&line);
		}
	}

	return list;
}

/* A text as lines of a byte array's initialiser, sixteen bytes a line, ending with the NUL. */
static sg_value *cbytes(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	sg_value *list = sg_value_list();
	sg_buf    line = {0};

	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		sg_value_free(list);
		return fail(error, "cbytes takes a text and no arguments");
	}

	size_t len = strlen(in->text);

	for (size_t i = 0; i <= len; ++i) {
		sg_buf_printf(&line, "%s0x%02x%s", line.len ? " " : "", (unsigned)(unsigned char)in->text[i], i < len ? "," : "");

		if ((i + 1) % 16 == 0 || i == len) {
			sg_list_push(list, sg_value_text_owned(line.data));
			memset(&line, 0, sizeof line);
		}
	}

	return list;
}

size_t sg_lang_c_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context) {
	const sg_filter filters[] = {
		{ "cname",    cname,    context },
		{ "storage",  storage,  context },
		{ "ctype",    ctype,    context },
		{ "idtype",   idtype,   context },
		{ "cstring",  cstring,  context },
		{ "cstrings", cstrings, context },
		{ "cbytes",   cbytes,   context },
	};
	size_t count = sizeof filters / sizeof filters[0];

	memcpy(out, filters, sizeof filters);
	return count;
}

bool sg_lang_c_check(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan) {
	bool ok = true;

	for (size_t i = 0; i < plan->custom_types.count; ++i) {
		const char *logical = plan->custom_types.items[i];
		bool        mapped  = false;

		for (size_t j = 0; j < target->types.count; ++j) {
			mapped = mapped || strcmp(target->types.items[j].key, logical) == 0;
		}

		if (!mapped) {
			sg_error(config->path, 0, 0, "$.targets[%zu].types maps no C type for the logical type %s",
					index, logical);
			ok = false;
		}
	}

	for (size_t t = 0; t < plan->tables.count; ++t) {
		const sg_plan_table *table = &plan->tables.items[t];

		for (size_t c = 0; c < table->columns.count; ++c) {
			const sg_plan_column *column = &table->columns.items[c];
			sg_affinity           a      = column->column->affinity;

			if (column->custom && a != SG_AFF_INTEGER && a != SG_AFF_REAL && !column->id_of) {
				sg_error(config->path, 0, 0, "%s.%s has the logical type %s, but a C type for a logical"
						" type is read and bound as an integer or a double, and the column is %s",
						table->table->name, column->column->name, column->type, sg_affinity_name(a));
				ok = false;
			}
		}
	}

	return ok;
}
