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

/* C++ adds these to C's (C's own _Bool and the like are reserved there too, and stay listed). */
static const char *const CPP_KEYWORDS[] = {
	"alignas", "alignof", "and", "and_eq", "asm", "bitand", "bitor", "catch", "char16_t", "char32_t",
	"char8_t", "class", "co_await", "co_return", "co_yield", "compl", "concept", "const_cast",
	"consteval", "constexpr", "constinit", "decltype", "delete", "dynamic_cast", "explicit", "export",
	"friend", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or",
	"or_eq", "private", "protected", "public", "reinterpret_cast", "requires", "static_assert",
	"static_cast", "template", "this", "thread_local", "throw", "try", "typeid", "typename", "using",
	"virtual", "wchar_t", "xor", "xor_eq", "final", "override", "std", NULL
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

static bool listed(const char *const *list, const char *word) {
	for (size_t i = 0; list[i]; ++i) {
		if (strcmp(word, list[i]) == 0) {
			return true;
		}
	}

	return false;
}

/* Snake case, `_` before a leading digit, `_` after a keyword of C (and of C++ when @p cpp). */
static sg_value *identifier(const sg_value *in, size_t count, bool cpp, char **error) {
	sg_buf out = {0};

	if (count || !snake(in, &out)) {
		sg_buf_free(&out);
		return fail(error, "%s takes a text or a list of words, and no arguments", cpp ? "cppname" : "cname");
	}

	if (!out.data) {
		return sg_value_text("_");
	}

	if (out.data[0] >= '0' && out.data[0] <= '9') {
		char *named = sg_format("_%s", out.data);

		sg_buf_free(&out);
		return sg_value_text_owned(named);
	}

	if (listed(KEYWORDS, out.data) || (cpp && listed(CPP_KEYWORDS, out.data))) {
		sg_buf_putc(&out, '_');
	}

	return sg_value_text_owned(out.data);
}

static sg_value *cname(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;
	return identifier(in, count, false, error);
}

static sg_value *cppname(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;
	return identifier(in, count, true, error);
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

static void pascal_word(const char *word, void *user) {
	sg_buf *out = user;

	sg_buf_putc(out, (char)(word[0] >= 'a' && word[0] <= 'z' ? word[0] - 'a' + 'A' : word[0]));
	sg_buf_puts(out, word + 1);
}

static void pascal(const sg_value *words, sg_buf *out) {
	for (size_t i = 0; words && i < words->items.count; ++i) {
		if (words->items.items[i]->kind == SG_V_TEXT) {
			sg_split_words(words->items.items[i]->text, pascal_word, out);
		}
	}
}

static bool is_cpp(const sg_lang_context *ctx) {
	return strcmp(ctx->target->language, "cpp") == 0;
}

/* C: <prefix>_<table>_<key column>. C++, inside the prefix's namespace: <Table><KeyColumn>. */
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

			if (is_cpp(ctx)) {
				pascal(sg_map_get(table, "words"), &out);
				pascal(sg_map_get(c, "words"), &out);
				continue;
			}

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

/* A column's C++ type: what its storage holds. A logical type from `types` is spelt by its storage;
 * a project wanting its own types changes the templates. */
static sg_value *cpptype(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	const sg_lang_context *ctx = user;

	(void)args;

	if (count || in->kind != SG_V_MAP) {
		return fail(error, "cpptype takes a column and no arguments");
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

	static const char *const TYPES[][2] = {
		{ "int", "int64_t" }, { "real", "double" }, { "text", "std::string" },
		{ "blob", "std::vector<unsigned char>" }, { "value", "Value" }
	};

	for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; ++i) {
		if (strcmp(kind, TYPES[i][0]) == 0) {
			return sg_value_text(TYPES[i][1]);
		}
	}

	return fail(error, "no C++ type for the storage %s", kind);
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

size_t sg_lang_cpp_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context) {
	const sg_filter filters[] = {
		{ "cppname",  cppname,  context },
		{ "storage",  storage,  context },
		{ "cpptype",  cpptype,  context },
		{ "idtype",   idtype,   context },
		{ "cstring",  cstring,  context },
		{ "cstrings", cstrings, context },
		{ "cbytes",   cbytes,   context },
	};
	size_t count = sizeof filters / sizeof filters[0];

	memcpy(out, filters, sizeof filters);
	return count;
}

typedef struct {
	char       *name;    //!< After the prefix and its underscore.
	const char *kind;    //!< What defines it, for the message: "table", "query" or "runtime".
	const char *owner;   //!< Its name; NULL for the runtime.
} c_name;

typedef SG_VEC(c_name) c_names;

static char *words_snake(const sg_words *words, const char *suffix) {
	sg_buf out = {0};

	for (size_t i = 0; i < words->count; ++i) {
		sg_buf_puts(&out, i ? "_" : "");
		sg_buf_puts(&out, words->items[i]);
	}

	sg_buf_puts(&out, suffix);
	return out.data;
}

static void add_name(c_names *names, char *name, const char *kind, const char *owner) {
	c_name *n = SG_PUSH(*names);

	n->name  = name;
	n->kind  = kind;
	n->owner = owner;
}

/* Every name the tables and the runtime define after the prefix, file names included. */
static void taken_names(const sg_plan *plan, c_names *out) {
	static const char *const RUNTIME[] = {
		"ctx", "ctx_open", "ctx_close", "ctx_db", "cursor", "cursor_close", "text", "text_free", "blob",
		"blob_free", "value", "value_free", "script", "patch", "systables", "patches", "patch_count",
		"schema_hash", "migrate", "internal", "scripts", NULL
	};
	static const char *const ROW[] = { "", "_row", "_row_free", "_read", "_next", NULL };

	for (size_t i = 0; RUNTIME[i]; ++i) {
		add_name(out, sg_xstrdup(RUNTIME[i]), "runtime", NULL);
	}

	for (size_t t = 0; t < plan->tables.count; ++t) {
		const sg_plan_table *table = &plan->tables.items[t];

		for (size_t i = 0; ROW[i]; ++i) {
			add_name(out, words_snake(&table->words, ROW[i]), "table", table->table->name);
		}

		for (size_t i = 0; i < table->statements.count; ++i) {
			char *suffix = words_snake(&table->statements.items[i].words, "");
			char *name   = sg_format("_%s", suffix);

			add_name(out, words_snake(&table->words, name), "table", table->table->name);
			free(name);
			free(suffix);
		}

		if (table->id_column >= 0) {
			char *column = words_snake(&table->columns.items[table->id_column].words, "");
			char *name   = sg_format("_%s", column);

			add_name(out, words_snake(&table->words, name), "table", table->table->name);
			free(name);
			free(column);
		}
	}
}

/* A query's function, and its row type and helpers unless it reuses a table's, must be new names. */
static bool distinct_queries(const sg_plan *plan, const sg_target *target) {
	static const char *const ROW[] = { "_row", "_row_free", "_read", "_next", NULL };
	c_names names = {0};
	bool    ok    = true;

	taken_names(plan, &names);

	for (size_t q = 0; plan->queries && q < plan->queries->count; ++q) {
		const sg_query *query = &plan->queries->items[q];
		size_t          first = names.count;

		add_name(&names, words_snake(&query->words, ""), "query", query->path);

		for (size_t i = 0; !query->row_table && ROW[i]; ++i) {
			add_name(&names, words_snake(&query->words, ROW[i]), "query", query->path);
		}

		/* The first clash says enough: the others follow from the same name. */
		bool clash = false;

		for (size_t mine = first; mine < names.count && !clash; ++mine) {
			for (size_t other = 0; other < first && !clash; ++other) {
				const c_name *taken = &names.items[other];

				if (strcmp(names.items[mine].name, taken->name) == 0) {
					sg_error(query->path, 0, 0, "the query would define %s_%s in C, which the %s%s%s already"
							" defines; rename the file", target->prefix, names.items[mine].name, taken->kind,
							taken->owner ? " " : "", taken->owner ? taken->owner : "");
					clash = true;
				}
			}
		}

		ok = ok && !clash;
	}

	for (size_t i = 0; i < names.count; ++i) {
		free(names.items[i].name);
	}

	free(names.items);
	return ok;
}

bool sg_lang_c_check(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan) {
	bool ok = distinct_queries(plan, target);

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

/* A table's or query's namespace: its words in snake case, as `cppname` spells them. */
static char *cpp_namespace(const sg_words *words) {
	char *name = words_snake(words, "");

	if (name[0] >= '0' && name[0] <= '9') {
		char *named = sg_format("_%s", name);

		free(name);
		name = named;
	}

	if (listed(KEYWORDS, name) || listed(CPP_KEYWORDS, name)) {
		char *named = sg_format("%s_", name);

		free(name);
		name = named;
	}

	return name;
}

bool sg_lang_cpp_check(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan) {
	static const char *const RUNTIME[] = {
		"detail", "migrate", "systables", "patches", "patch_count", "schema_hash", NULL
	};
	c_names names = {0};
	bool    ok    = true;

	(void)config;
	(void)index;

	for (size_t i = 0; RUNTIME[i]; ++i) {
		add_name(&names, sg_xstrdup(RUNTIME[i]), "runtime", NULL);
	}

	for (size_t t = 0; t < plan->tables.count; ++t) {
		add_name(&names, cpp_namespace(&plan->tables.items[t].words), "table", plan->tables.items[t].table->name);
	}

	for (size_t q = 0; plan->queries && q < plan->queries->count; ++q) {
		add_name(&names, cpp_namespace(&plan->queries->items[q].words), "query", plan->queries->items[q].path);
	}

	/* Two tables can differ only in the case of a letter, which their words do not keep. */
	for (size_t i = 0; i < names.count; ++i) {
		for (size_t j = 0; j < i; ++j) {
			if (strcmp(names.items[i].name, names.items[j].name) == 0 && strcmp(names.items[i].kind, "runtime") != 0) {
				const c_name *taken = &names.items[j];

				sg_error(strcmp(names.items[i].kind, "query") == 0 ? names.items[i].owner : NULL, 0, 0,
						"the %s %s would be the namespace %s::%s in C++, which the %s%s%s already uses",
						names.items[i].kind, names.items[i].owner, target->prefix, names.items[i].name, taken->kind,
						taken->owner ? " " : "", taken->owner ? taken->owner : "");
				ok = false;
				break;
			}
		}
	}

	for (size_t i = 0; i < names.count; ++i) {
		free(names.items[i].name);
	}

	free(names.items);
	return ok;
}
