#include "config.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "json.h"

enum {
	K_TEXT       = 1 << 0,
	K_BOOL       = 1 << 1,
	K_TEXT_ARRAY = 1 << 2,
	K_OBJECT     = 1 << 3,
	K_NULL       = 1 << 4,
	K_OBJECTS    = 1 << 5     /* an array of objects */
};

typedef struct {
	const char *name;
	unsigned    kinds;
} field;

typedef struct {
	sqlite3    *db;
	const char *text;
	int         len;
	const char *file;
} reader;

static const field TOP[] = {
	{ "schema",   K_OBJECT },
	{ "generate", K_OBJECT },
	{ "targets",  K_OBJECTS },
	{ "types",    K_OBJECT },
	{ "tables",  K_OBJECT },
	{ "rules",   K_OBJECT },
	{ "queries", K_TEXT_ARRAY },
	{ NULL, 0 }
};

static const field SCHEMA[] = {
	{ "systables",             K_TEXT | K_NULL },
	{ "patches",               K_TEXT | K_TEXT_ARRAY },
	{ "double_quoted_strings", K_BOOL },
	{ "foreign_keys",          K_BOOL },
	{ NULL, 0 }
};

static const field GENERATE[] = {
	{ "embed_scripts", K_BOOL },
	{ "typed_ids",     K_BOOL },
	{ NULL, 0 }
};

/* The keys of a target: the ones every language takes, then each language's own. */
static const field TARGET_C[] = {
	{ "language",      K_TEXT },
	{ "dir",           K_TEXT },
	{ "templates",     K_TEXT | K_NULL },
	{ "confirm_clear", K_BOOL },
	{ "types",         K_OBJECT },
	{ "prefix",        K_TEXT },
	{ NULL, 0 }
};

/* Documentation: no names to spell, no types to map. */
static const field TARGET_DOC[] = {
	{ "language",      K_TEXT },
	{ "dir",           K_TEXT },
	{ "templates",     K_TEXT | K_NULL },
	{ "confirm_clear", K_BOOL },
	{ NULL, 0 }
};

/* Each language's own directory when a target names none, so targets of different languages
 * never share one by default. The documentation gets directories of its own inside docs/, since a
 * target clears its directory. */
static const struct {
	const char  *name;
	const field *keys;
	const char  *dir;
} LANGUAGES[] = {
	{ "c",        TARGET_C,   "gen" },
	{ "markdown", TARGET_DOC, "docs/schema" },
	{ "mermaid",  TARGET_DOC, "docs/diagram" },
	{ NULL, NULL, NULL }
};

#define KNOWN_LANGUAGES "c, markdown, mermaid"

static const field TYPES[] = {
	{ "by_name",     K_OBJECT },
	{ "by_declared", K_OBJECT },
	{ NULL, 0 }
};

static const field TABLE[] = {
	{ "writes", K_BOOL | K_TEXT_ARRAY },
	{ NULL, 0 }
};

static const struct {
	const char *name;
	unsigned    flag;
} WRITES[] = {
	{ "insert", SG_WRITE_INSERT },
	{ "update", SG_WRITE_UPDATE },
	{ "delete", SG_WRITE_DELETE },
	{ "upsert", SG_WRITE_UPSERT },
	{ "link",   SG_WRITE_LINK },
	{ NULL, 0 }
};

/* json_each quotes some keys in fullkey ($.schema."foreign_keys"); drop quotes no key needs. */
static char *pretty(const char *fullkey) {
	sg_buf out = {0};

	for (const char *c = fullkey; *c; ++c) {
		if (*c == '"') {
			const char *end   = strchr(c + 1, '"');
			bool        plain = end != NULL && end > c + 1;

			for (const char *k = c + 1; plain && k < end; ++k) {
				plain = (*k >= 'a' && *k <= 'z') || (*k >= 'A' && *k <= 'Z') || (*k >= '0' && *k <= '9')
						|| *k == '_';
			}

			if (plain) {
				sg_buf_append(&out, c + 1, (size_t)(end - c - 1));
				c = end;
				continue;
			}
		}

		sg_buf_putc(&out, *c);
	}

	return out.data ? out.data : sg_xstrdup("");
}

/* Report a problem at a JSON path. */
static void problem(reader *r, const char *fullkey, const char *fmt, const char *a, const char *b) {
	char *path = pretty(fullkey);
	char *text = sg_format(fmt, a, b);

	sg_error(r->file, 0, 0, "%s%s", path, text);
	free(text);
	free(path);
}

enum { MAX_PARAMS = 3 };

/* Every rule sqlgen knows, its default level, and its parameters with their defaults; a parameter
 * whose default is NULL is required while the rule is on. Rules about SQLite's own semantics are
 * on; rules about one project's conventions are off until that project's file switches them on. */
static const struct {
	const char *name;
	sg_level    level;
	struct {
		const char *name;
		const char *fallback;
	} params[MAX_PARAMS + 1];
} RULES[] = {
	{ "fk_target",                   SG_LEVEL_ERROR, { { NULL, NULL } } },
	{ "fk_indexed",                  SG_LEVEL_WARN,  { { NULL, NULL } } },
	{ "nullable_unique",             SG_LEVEL_WARN,  { { NULL, NULL } } },
	{ "strict",                      SG_LEVEL_OFF,   { { NULL, NULL } } },
	{ "pk_shape",                    SG_LEVEL_OFF,   { { NULL, NULL } } },
	{ "pk_naming",                   SG_LEVEL_OFF,   { { "autoincrement", "serial" }, { "plain", "id" }, { NULL, NULL } } },
	{ "created_at",                  SG_LEVEL_OFF,   { { "column", "created_at" }, { NULL, NULL } } },
	{ "updated_at_needs_created_at", SG_LEVEL_OFF,   { { "updated", "updated_at" }, { "created", "created_at" }, { NULL, NULL } } },
	{ "stamp_type",                  SG_LEVEL_OFF,   { { "pattern", "*_at" }, { NULL, NULL } } },
	{ "prefix_origin",               SG_LEVEL_OFF,   { { "prefix", NULL }, { NULL, NULL } } },
	{ "query_scan",                  SG_LEVEL_WARN,  { { NULL, NULL } } },
	{ "drop_table",                  SG_LEVEL_WARN,  { { NULL, NULL } } },
	{ "drop_column",                 SG_LEVEL_WARN,  { { NULL, NULL } } },
	{ "column_type",                 SG_LEVEL_WARN,  { { NULL, NULL } } },
	{ "not_null_added",              SG_LEVEL_WARN,  { { NULL, NULL } } },
	{ "unique_added",                SG_LEVEL_OFF,   { { NULL, NULL } } },
	{ "rename",                      SG_LEVEL_OFF,   { { NULL, NULL } } },
	{ NULL, SG_LEVEL_OFF, { { NULL, NULL } } }
};

/* The defaults of every other key, used both when a key is absent and in the written file. */
#define DEFAULT_PATCHES               "sql/*/"
#define DEFAULT_DOUBLE_QUOTED_STRINGS false
#define DEFAULT_FOREIGN_KEYS          true
#define DEFAULT_CONFIRM_CLEAR         true
#define DEFAULT_PREFIX                "db"
#define DEFAULT_EMBED_SCRIPTS         true
#define DEFAULT_TYPED_IDS             true

/* Plain JSON has no comments, so the file states every key with its value instead. */
static const char DEFAULT_TEXT[] =
	"{\n"
	"\t\"schema\": {\n"
	"\t\t\"systables\": null,\n"
	"\t\t\"patches\": \"" DEFAULT_PATCHES "\",\n"
	"\t\t\"double_quoted_strings\": false,\n"
	"\t\t\"foreign_keys\": true\n"
	"\t},\n"
	"\t\"generate\": {\n"
	"\t\t\"embed_scripts\": true,\n"
	"\t\t\"typed_ids\": true\n"
	"\t},\n"
	"\t\"targets\": [\n"
	"\t\t{\n"
	"\t\t\t\"language\": \"c\",\n"
	"\t\t\t\"dir\": \"gen\",\n"
	"\t\t\t\"prefix\": \"" DEFAULT_PREFIX "\",\n"
	"\t\t\t\"templates\": null,\n"
	"\t\t\t\"confirm_clear\": true,\n"
	"\t\t\t\"types\": {}\n"
	"\t\t},\n"
	"\t\t{\n"
	"\t\t\t\"language\": \"markdown\",\n"
	"\t\t\t\"dir\": \"docs/schema\",\n"
	"\t\t\t\"templates\": null,\n"
	"\t\t\t\"confirm_clear\": true\n"
	"\t\t},\n"
	"\t\t{\n"
	"\t\t\t\"language\": \"mermaid\",\n"
	"\t\t\t\"dir\": \"docs/diagram\",\n"
	"\t\t\t\"templates\": null,\n"
	"\t\t\t\"confirm_clear\": true\n"
	"\t\t}\n"
	"\t],\n"
	"\t\"types\": {\n"
	"\t\t\"by_declared\": {},\n"
	"\t\t\"by_name\": {}\n"
	"\t},\n"
	"\t\"tables\": {},\n"
	"\t\"rules\": {\n"
	"\t\t\"fk_target\": \"error\",\n"
	"\t\t\"fk_indexed\": \"warn\",\n"
	"\t\t\"nullable_unique\": \"warn\",\n"
	"\t\t\"strict\": \"off\",\n"
	"\t\t\"pk_shape\": \"off\",\n"
	"\t\t\"pk_naming\": { \"level\": \"off\", \"autoincrement\": \"serial\", \"plain\": \"id\" },\n"
	"\t\t\"created_at\": { \"level\": \"off\", \"column\": \"created_at\" },\n"
	"\t\t\"updated_at_needs_created_at\": { \"level\": \"off\", \"updated\": \"updated_at\", \"created\": \"created_at\" },\n"
	"\t\t\"stamp_type\": { \"level\": \"off\", \"pattern\": \"*_at\" },\n"
	"\t\t\"prefix_origin\": { \"level\": \"off\", \"prefix\": null },\n"
	"\t\t\"query_scan\": \"warn\",\n"
	"\t\t\"drop_table\": \"warn\",\n"
	"\t\t\"drop_column\": \"warn\",\n"
	"\t\t\"column_type\": \"warn\",\n"
	"\t\t\"not_null_added\": \"warn\",\n"
	"\t\t\"unique_added\": \"off\",\n"
	"\t\t\"rename\": \"off\"\n"
	"\t},\n"
	"\t\"queries\": []\n"
	"}\n";

const char *sg_config_default_text(void) {
	return DEFAULT_TEXT;
}

static sqlite3_stmt *prepare(reader *r, const char *sql, const char *path) {
	sqlite3_stmt *stmt = NULL;

	if (sqlite3_prepare_v2(r->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		sg_error(r->file, 0, 0, "internal: %s", sqlite3_errmsg(r->db));
		return NULL;
	}

	sqlite3_bind_text(stmt, 1, r->text, r->len, SQLITE_STATIC);

	if (path) {
		sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
	}

	return stmt;
}

/* The JSON type at a path, or NULL when nothing is there. */
static char *type_at(reader *r, const char *path) {
	sqlite3_stmt *stmt = prepare(r, "select json_type(?1, ?2)", path);
	char         *type = NULL;

	if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		type = sg_xstrdup((const char *)sqlite3_column_text(stmt, 0));
	}

	sqlite3_finalize(stmt);
	return type;
}

/* A text value, a JSON rendering of a container, or NULL. */
static char *value_at(reader *r, const char *path) {
	sqlite3_stmt *stmt  = prepare(r, "select json_extract(?1, ?2)", path);
	char         *value = NULL;

	if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		value = sg_xstrdup((const char *)sqlite3_column_text(stmt, 0));
	}

	sqlite3_finalize(stmt);
	return value;
}

/* A string value; NULL when absent or of another type, which validation reports. */
static char *text_at(reader *r, const char *path) {
	char *type  = type_at(r, path);
	char *value = type && strcmp(type, "text") == 0 ? value_at(r, path) : NULL;

	free(type);
	return value;
}

static sqlite3_stmt *each(reader *r, const char *path) {
	return prepare(r, "select key, fullkey, type from json_each(?1, ?2)", path);
}

static const char *describe(unsigned kinds) {
	switch (kinds) {
	case K_TEXT:                return "a string";
	case K_BOOL:                return "true or false";
	case K_OBJECT:              return "an object";
	case K_TEXT_ARRAY:          return "an array of strings";
	case K_TEXT | K_TEXT_ARRAY: return "a string or an array of strings";
	case K_BOOL | K_TEXT_ARRAY: return "true, false or an array of strings";
	case K_TEXT | K_NULL:       return "a string or null";
	case K_OBJECTS:             return "an array of objects";
	default:                    return "something else";
	}
}

static bool check_text_array(reader *r, const char *path) {
	sqlite3_stmt *stmt = each(r, path);
	bool          ok   = stmt != NULL;

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *type = (const char *)sqlite3_column_text(stmt, 2);

		if (strcmp(type, "text") != 0) {
			problem(r, (const char *)sqlite3_column_text(stmt, 1), " must be a string, not %s", type, NULL);
			ok = false;
		}
	}

	sqlite3_finalize(stmt);
	return ok;
}

static bool check_object_array(reader *r, const char *path) {
	sqlite3_stmt *stmt = each(r, path);
	bool          ok   = stmt != NULL;

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *type = (const char *)sqlite3_column_text(stmt, 2);

		if (strcmp(type, "object") != 0) {
			problem(r, (const char *)sqlite3_column_text(stmt, 1), " must be an object, not %s", type, NULL);
			ok = false;
		}
	}

	sqlite3_finalize(stmt);
	return ok;
}

static bool kind_matches(reader *r, const char *fullkey, const char *type, unsigned kinds) {
	if (strcmp(type, "text") == 0) {
		return (kinds & K_TEXT) != 0;
	}

	if (strcmp(type, "true") == 0 || strcmp(type, "false") == 0) {
		return (kinds & K_BOOL) != 0;
	}

	if (strcmp(type, "object") == 0) {
		return (kinds & K_OBJECT) != 0;
	}

	if (strcmp(type, "null") == 0) {
		return (kinds & K_NULL) != 0;
	}

	if (strcmp(type, "array") == 0) {
		return ((kinds & K_TEXT_ARRAY) != 0 && check_text_array(r, fullkey))
				|| ((kinds & K_OBJECTS) != 0 && check_object_array(r, fullkey));
	}

	return false;
}

/* Every key of the object at @p path must be one of @p fields, with a value of an allowed kind. */
static bool check_object(reader *r, const char *path, const field *fields) {
	sqlite3_stmt *stmt = each(r, path);
	bool          ok   = stmt != NULL;

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char  *key     = (const char *)sqlite3_column_text(stmt, 0);
		const char  *fullkey = (const char *)sqlite3_column_text(stmt, 1);
		const char  *type    = (const char *)sqlite3_column_text(stmt, 2);
		const field *match   = NULL;

		for (const field *f = fields; f->name; ++f) {
			if (strcmp(f->name, key) == 0) {
				match = f;
				break;
			}
		}

		bool elements = strcmp(type, "array") == 0 && (match ? match->kinds & (K_TEXT_ARRAY | K_OBJECTS) : 0);

		if (!match) {
			problem(r, fullkey, ": unknown key", NULL, NULL);
			ok = false;
		} else if (elements) {
			/* Each element in the wrong shape has been reported where it is. */
			ok = kind_matches(r, fullkey, type, match->kinds) && ok;
		} else if (!kind_matches(r, fullkey, type, match->kinds)) {
			problem(r, fullkey, " must be %s, not %s", describe(match->kinds), type);
			ok = false;
		}
	}

	sqlite3_finalize(stmt);
	return ok;
}

static bool flag_at(reader *r, const char *path, bool fallback) {
	char *type  = type_at(r, path);
	bool  value = type ? strcmp(type, "true") == 0 : fallback;

	free(type);
	return value;
}

static void push_texts(reader *r, const char *path, const char *dir, sg_str_vec *out) {
	char *type = type_at(r, path);

	if (type && strcmp(type, "text") == 0) {
		char *value = value_at(r, path);

		*SG_PUSH(*out) = sg_path_join(dir, value);
		free(value);
	} else if (type && strcmp(type, "array") == 0) {
		sqlite3_stmt *stmt = prepare(r, "select value from json_each(?1, ?2)", path);

		while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
			*SG_PUSH(*out) = sg_path_join(dir, (const char *)sqlite3_column_text(stmt, 0));
		}

		sqlite3_finalize(stmt);
	}

	free(type);
}

static bool is_identifier(const char *text) {
	if (!text || !(text[0] == '_' || (text[0] >= 'a' && text[0] <= 'z')
			|| (text[0] >= 'A' && text[0] <= 'Z'))) {
		return false;
	}

	for (const char *c = text; *c; ++c) {
		if (!(*c == '_' || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
				|| (*c >= '0' && *c <= '9'))) {
			return false;
		}
	}

	return true;
}

/* An object whose every value is a string, collected as pairs. With @p logical, every value must
 * be a logical type name (an identifier); with @p logical_keys, every key must be one. */
static bool read_pairs(reader *r, const char *path, sg_pair_vec *pairs, bool logical, bool logical_keys) {
	sqlite3_stmt *stmt = prepare(r, "select key, fullkey, type, value from json_each(?1, ?2)", path);
	bool          ok   = stmt != NULL;

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *type = (const char *)sqlite3_column_text(stmt, 2);

		if (strcmp(type, "text") != 0) {
			problem(r, (const char *)sqlite3_column_text(stmt, 1), " must be a string, not %s", type, NULL);
			ok = false;
			continue;
		}

		const char *key   = (const char *)sqlite3_column_text(stmt, 0);
		const char *value = (const char *)sqlite3_column_text(stmt, 3);

		if (logical && !is_identifier(value)) {
			problem(r, (const char *)sqlite3_column_text(stmt, 1), ": '%s' is not a logical type name;"
					" use letters, digits and _", value, NULL);
			ok = false;
			continue;
		}

		if (logical_keys && !is_identifier(key)) {
			problem(r, (const char *)sqlite3_column_text(stmt, 1), ": '%s' is not a logical type name;"
					" use letters, digits and _", key, NULL);
			ok = false;
			continue;
		}

		if (value[0] == '\0') {
			problem(r, (const char *)sqlite3_column_text(stmt, 1), " must not be empty", NULL, NULL);
			ok = false;
			continue;
		}

		sg_pair *pair = SG_PUSH(*pairs);

		pair->key   = sg_xstrdup(key);
		pair->value = sg_xstrdup(value);
	}

	sqlite3_finalize(stmt);
	return ok;
}

static bool read_tables(reader *r, sg_config *config) {
	sqlite3_stmt *stmt = each(r, "$.tables");
	bool          ok   = stmt != NULL;

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *key     = (const char *)sqlite3_column_text(stmt, 0);
		const char *fullkey = (const char *)sqlite3_column_text(stmt, 1);
		const char *type    = (const char *)sqlite3_column_text(stmt, 2);

		if (strcmp(type, "object") != 0) {
			problem(r, fullkey, " must be an object, not %s", type, NULL);
			ok = false;
			continue;
		}

		if (!check_object(r, fullkey, TABLE)) {
			ok = false;
			continue;
		}

		sg_table_config  *table   = SG_PUSH(config->tables);
		sg_table_options *options = &table->options;
		char             *path    = sg_format("%s.writes", fullkey);
		char             *wtype   = type_at(r, path);

		table->name         = sg_xstrdup(key);
		options->has_writes = wtype != NULL;

		if (wtype && strcmp(wtype, "true") == 0) {
			options->writes = SG_WRITE_ALL;
		} else if (wtype && strcmp(wtype, "array") == 0) {
			sqlite3_stmt *items = prepare(r, "select fullkey, value from json_each(?1, ?2)", path);

			while (items && sqlite3_step(items) == SQLITE_ROW) {
				const char *name  = (const char *)sqlite3_column_text(items, 1);
				unsigned    flag  = 0;

				for (size_t i = 0; WRITES[i].name; ++i) {
					if (strcmp(WRITES[i].name, name) == 0) {
						flag = WRITES[i].flag;
					}
				}

				if (!flag) {
					problem(r, (const char *)sqlite3_column_text(items, 0), ": unknown write '%s';"
							" expected insert, update, delete, upsert or link", name, NULL);
					ok = false;
				}

				options->writes |= flag;
			}

			sqlite3_finalize(items);
		}

		free(wtype);
		free(path);
	}

	sqlite3_finalize(stmt);
	return ok;
}

static bool parse_level(const char *text, sg_level *out) {
	if (text && strcmp(text, "error") == 0) {
		*out = SG_LEVEL_ERROR;
	} else if (text && strcmp(text, "warn") == 0) {
		*out = SG_LEVEL_WARN;
	} else if (text && strcmp(text, "off") == 0) {
		*out = SG_LEVEL_OFF;
	} else {
		return false;
	}

	return true;
}

static void default_rules(sg_config *config) {
	for (size_t i = 0; RULES[i].name; ++i) {
		sg_rule_config *rule = SG_PUSH(config->rules);

		rule->name  = RULES[i].name;
		rule->level = RULES[i].level;

		for (size_t p = 0; RULES[i].params[p].name; ++p) {
			sg_pair *param = SG_PUSH(rule->params);

			param->key   = sg_xstrdup(RULES[i].params[p].name);
			param->value = sg_xstrdup(RULES[i].params[p].fallback);
		}
	}
}

/* The members of a rule's object other than "level": each must be one of its parameters, holding a
 * string or null. */
static bool read_params(reader *r, const char *fullkey, sg_rule_config *rule) {
	sqlite3_stmt *stmt = prepare(r, "select key, fullkey, type, value from json_each(?1, ?2)", fullkey);
	bool          ok   = stmt != NULL;

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *key   = (const char *)sqlite3_column_text(stmt, 0);
		const char *path  = (const char *)sqlite3_column_text(stmt, 1);
		const char *type  = (const char *)sqlite3_column_text(stmt, 2);
		sg_pair    *param = NULL;

		if (strcmp(key, "level") == 0) {
			continue;
		}

		for (size_t i = 0; i < rule->params.count && !param; ++i) {
			if (strcmp(rule->params.items[i].key, key) == 0) {
				param = &rule->params.items[i];
			}
		}

		if (!param) {
			problem(r, path, rule->params.count ? ": unknown parameter" : ": unknown parameter;"
					" this rule takes none", NULL, NULL);
			ok = false;
		} else if (strcmp(type, "text") != 0 && strcmp(type, "null") != 0) {
			problem(r, path, " must be a string or null, not %s", type, NULL);
			ok = false;
		} else {
			free(param->value);
			param->value = sg_xstrdup((const char *)sqlite3_column_text(stmt, 3));
		}
	}

	sqlite3_finalize(stmt);
	return ok;
}

/* A parameter without a default must be set while its rule is on. */
static bool check_required(reader *r, const sg_config *config) {
	bool ok = true;

	for (size_t i = 0; i < config->rules.count; ++i) {
		const sg_rule_config *rule = &config->rules.items[i];

		for (size_t p = 0; rule->level != SG_LEVEL_OFF && p < rule->params.count; ++p) {
			if (!rule->params.items[p].value) {
				sg_error(r->file, 0, 0, "$.rules.%s.%s is required while the rule is on", rule->name,
						rule->params.items[p].key);
				ok = false;
			}
		}
	}

	return ok;
}

/* Rule names, levels and parameters. */
static bool read_rules(reader *r, sg_config *config) {
	sqlite3_stmt *stmt = prepare(r, "select key, fullkey, type, value from json_each(?1, ?2)",
			"$.rules");
	bool          ok   = stmt != NULL;

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char     *key     = (const char *)sqlite3_column_text(stmt, 0);
		const char     *fullkey = (const char *)sqlite3_column_text(stmt, 1);
		const char     *type    = (const char *)sqlite3_column_text(stmt, 2);
		const char     *value   = (const char *)sqlite3_column_text(stmt, 3);
		sg_rule_config *rule    = NULL;
		char           *level   = NULL;
		sg_level        parsed  = SG_LEVEL_OFF;

		for (size_t i = 0; i < config->rules.count && !rule; ++i) {
			if (strcmp(config->rules.items[i].name, key) == 0) {
				rule = &config->rules.items[i];
			}
		}

		if (!rule) {
			problem(r, fullkey, ": unknown rule", NULL, NULL);
			ok = false;
			continue;
		}

		if (strcmp(type, "text") == 0) {
			level = sg_xstrdup(value);
		} else if (strcmp(type, "object") == 0) {
			char *path = sg_format("%s.level", fullkey);

			level = text_at(r, path);
			free(path);
			ok = read_params(r, fullkey, rule) && ok;
		} else {
			problem(r, fullkey, " must be a level or an object, not %s", type, NULL);
			ok = false;
			continue;
		}

		if (!parse_level(level, &parsed)) {
			problem(r, fullkey, ": level must be \"error\", \"warn\" or \"off\"", NULL, NULL);
			free(level);
			ok = false;
			continue;
		}

		free(level);
		rule->level = parsed;
	}

	sqlite3_finalize(stmt);
	return ok;
}

/* Strict JSON first: SQLite's functions would also accept JSON5, which other tools would not. */
static bool syntax_ok(reader *r, size_t length) {
	sg_json_problem problem;
	unsigned        line;
	unsigned        col;

	if (sg_json_check(r->text, length, &problem)) {
		return true;
	}

	sg_line_col(r->text, problem.offset, &line, &col);
	sg_error(r->file, line, col, "%s", problem.reason);
	return false;
}

/* A directory as targets are compared: no `.` segments, no doubled or trailing slashes. */
static char *normalized_dir(const char *path) {
	sg_buf out = {0};

	for (const char *s = path; *s; ) {
		size_t n = strcspn(s, "/");

		if (n == 0 && s == path) {
			sg_buf_putc(&out, '/');
		} else if (n > 0 && !(n == 1 && s[0] == '.')) {
			if (out.len && out.data[out.len - 1] != '/') {
				sg_buf_putc(&out, '/');
			}

			sg_buf_append(&out, s, n);
		}

		s += n + (s[n] == '/');
	}

	return out.data ? out.data : sg_xstrdup(".");
}

static char *target_dir(const sg_config *config, const char *dir) {
	char *joined = sg_path_join(config->dir, dir);
	char *clean  = normalized_dir(joined);

	free(joined);
	return clean;
}

/* With no `targets`, one target per language, as the written file lists them. */
static void default_targets(sg_config *config) {
	for (size_t i = 0; LANGUAGES[i].name; ++i) {
		sg_target *target = SG_PUSH(config->targets);

		target->language      = sg_xstrdup(LANGUAGES[i].name);
		target->dir           = target_dir(config, LANGUAGES[i].dir);
		target->prefix        = strcmp(LANGUAGES[i].name, "c") == 0 ? sg_xstrdup(DEFAULT_PREFIX) : NULL;
		target->confirm_clear = DEFAULT_CONFIRM_CLEAR;
	}
}

static bool read_target(reader *r, const char *fullkey, sg_config *config) {
	char        *path     = sg_format("%s.language", fullkey);
	char        *language = text_at(r, path);
	const field *keys     = NULL;
	const char  *fallback = NULL;
	bool         ok       = true;

	free(path);

	for (size_t i = 0; language && LANGUAGES[i].name; ++i) {
		if (strcmp(LANGUAGES[i].name, language) == 0) {
			keys     = LANGUAGES[i].keys;
			fallback = LANGUAGES[i].dir;
		}
	}

	if (!language) {
		problem(r, fullkey, ": language is required", NULL, NULL);
		return false;
	}

	if (!keys) {
		problem(r, fullkey, ": unknown language '%s'; known: " KNOWN_LANGUAGES, language, NULL);
		free(language);
		return false;
	}

	if (!check_object(r, fullkey, keys)) {
		free(language);
		return false;
	}

	sg_target *target = SG_PUSH(config->targets);
	char      *key;

	target->language = language;

	key = sg_format("%s.dir", fullkey);
	char *dir = text_at(r, key);
	/* "out", "out/" and "./out" are one directory, which the check for shared directories has to see. */
	target->dir = target_dir(config, dir ? dir : fallback);
	free(dir);
	free(key);

	key = sg_format("%s.templates", fullkey);
	char *templates = text_at(r, key);
	target->templates = templates ? sg_path_join(config->dir, templates) : NULL;
	free(templates);
	free(key);

	key = sg_format("%s.confirm_clear", fullkey);
	target->confirm_clear = flag_at(r, key, DEFAULT_CONFIRM_CLEAR);
	free(key);

	key = sg_format("%s.types", fullkey);
	char *types = type_at(r, key);
	if (types && strcmp(types, "object") == 0) {
		ok = read_pairs(r, key, &target->types, false, true) && ok;
	}
	free(types);
	free(key);

	if (strcmp(language, "c") == 0) {
		key = sg_format("%s.prefix", fullkey);
		target->prefix = text_at(r, key);

		if (!target->prefix) {
			target->prefix = sg_xstrdup(DEFAULT_PREFIX);
		} else if (!is_identifier(target->prefix)) {
			problem(r, key, ": '%s' is not a C identifier", target->prefix, NULL);
			ok = false;
		}

		free(key);
	}

	return ok;
}

/* Each target clears its directory, so two sharing one would delete each other's files. */
static bool read_targets(reader *r, sg_config *config, bool present) {
	bool ok = true;

	if (!present) {
		default_targets(config);
		return true;
	}

	sqlite3_stmt *stmt = each(r, "$.targets");

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		if (strcmp((const char *)sqlite3_column_text(stmt, 2), "object") == 0) {
			ok = read_target(r, (const char *)sqlite3_column_text(stmt, 1), config) && ok;
		}
	}

	sqlite3_finalize(stmt);

	/* Each target clears its directory, so none may hold another's. */
	for (size_t i = 0; i < config->targets.count; ++i) {
		for (size_t j = 0; j < config->targets.count; ++j) {
			const char *a   = config->targets.items[i].dir;
			const char *b   = config->targets.items[j].dir;
			size_t      len = strlen(a);

			if (i < j && strcmp(a, b) == 0) {
				sg_error(r->file, 0, 0, "$.targets[%zu] and $.targets[%zu] both write to %s; each target"
						" clears its directory", i, j, a);
				ok = false;
			} else if (i != j && strncmp(a, b, len) == 0 && (b[len] == '/' || strcmp(a, "/") == 0) && b[len]) {
				sg_error(r->file, 0, 0, "$.targets[%zu] writes to %s, inside the directory of $.targets[%zu],"
						" %s, which that target clears", j, b, i, a);
				ok = false;
			}
		}
	}

	return ok;
}

static bool validate(reader *r) {
	char *top = type_at(r, "$");
	bool  ok  = true;

	if (!top || strcmp(top, "object") != 0) {
		sg_error(r->file, 0, 0, "must hold a JSON object");
		free(top);
		return false;
	}

	free(top);

	ok = check_object(r, "$", TOP) && ok;

	char *schema = type_at(r, "$.schema");

	if (schema && strcmp(schema, "object") == 0) {
		ok = check_object(r, "$.schema", SCHEMA) && ok;
	}

	free(schema);

	char *output = type_at(r, "$.generate");
	char *types  = type_at(r, "$.types");

	if (output && strcmp(output, "object") == 0) {
		ok = check_object(r, "$.generate", GENERATE) && ok;
	}

	if (types && strcmp(types, "object") == 0) {
		ok = check_object(r, "$.types", TYPES) && ok;
	}

	free(output);
	free(types);
	return ok;
}

static bool read_values(reader *r, sg_config *config) {
	bool  ok        = true;
	char *systables = text_at(r, "$.schema.systables");

	if (systables) {
		config->systables = sg_path_join(config->dir, systables);
		free(systables);
	}

	char *patches = type_at(r, "$.schema.patches");

	if (patches) {
		push_texts(r, "$.schema.patches", config->dir, &config->patch_patterns);
	} else {
		*SG_PUSH(config->patch_patterns) = sg_path_join(config->dir, DEFAULT_PATCHES);
	}

	free(patches);

	/* Only reachable by stating `patches: []` with no systables. */
	if (!config->systables && config->patch_patterns.count == 0) {
		sg_error(r->file, 0, 0, "$.schema names no scripts; give systables, patches or both");
		ok = false;
	}

	config->double_quoted_strings = flag_at(r, "$.schema.double_quoted_strings",
			DEFAULT_DOUBLE_QUOTED_STRINGS);
	config->foreign_keys          = flag_at(r, "$.schema.foreign_keys", DEFAULT_FOREIGN_KEYS);

	config->embed_scripts = flag_at(r, "$.generate.embed_scripts", DEFAULT_EMBED_SCRIPTS);
	config->typed_ids     = flag_at(r, "$.generate.typed_ids", DEFAULT_TYPED_IDS);

	char *targets = type_at(r, "$.targets");

	ok = read_targets(r, config, targets && strcmp(targets, "array") == 0) && ok;
	free(targets);

	char *by_name     = type_at(r, "$.types.by_name");
	char *by_declared = type_at(r, "$.types.by_declared");

	if (by_name && strcmp(by_name, "object") == 0) {
		ok = read_pairs(r, "$.types.by_name", &config->types_by_name, true, false) && ok;
	}

	if (by_declared && strcmp(by_declared, "object") == 0) {
		ok = read_pairs(r, "$.types.by_declared", &config->types_by_declared, true, false) && ok;
	}

	free(by_name);
	free(by_declared);

	char *tables = type_at(r, "$.tables");
	char *rules  = type_at(r, "$.rules");

	if (tables && strcmp(tables, "object") == 0) {
		ok = read_tables(r, config) && ok;
	}

	default_rules(config);

	if (rules && strcmp(rules, "object") == 0) {
		ok = read_rules(r, config) && ok;
	}

	ok = check_required(r, config) && ok;

	free(tables);
	free(rules);
	push_texts(r, "$.queries", config->dir, &config->queries);
	return ok;
}

bool sg_config_load_text(const char *name, const char *text, size_t len, sg_config *out) {
	reader r  = {0};
	bool   ok = false;

	memset(out, 0, sizeof *out);
	out->path = sg_xstrdup(name);
	out->dir  = sg_dirname(name);

	if (len > INT_MAX) {
		sg_error(name, 0, 0, "is too large");
		return false;
	}

	if (sqlite3_open_v2(":memory:", &r.db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		sg_error(NULL, 0, 0, "a scratch database could not be opened");
		sqlite3_close(r.db);
		return false;
	}

	r.text = text;
	r.len  = (int)len;
	r.file = name;

	if (syntax_ok(&r, len)) {
		bool valid = validate(&r);

		ok = read_values(&r, out) && valid;
	}

	sqlite3_close(r.db);
	return ok;
}

bool sg_config_load(const char *path, sg_config *out) {
	char   *text = NULL;
	size_t  len  = 0;

	if (!sg_read_file(path, &text, &len)) {
		memset(out, 0, sizeof *out);
		return false;
	}

	bool ok = sg_config_load_text(path, text, len, out);

	free(text);
	return ok;
}

void sg_config_free(sg_config *config) {
	free(config->path);
	free(config->dir);
	free(config->systables);

	for (size_t i = 0; i < config->patch_patterns.count; ++i) {
		free(config->patch_patterns.items[i]);
	}

	free(config->patch_patterns.items);
	for (size_t i = 0; i < config->targets.count; ++i) {
		sg_target *target = &config->targets.items[i];

		free(target->language);
		free(target->dir);
		free(target->prefix);
		free(target->templates);

		for (size_t p = 0; p < target->types.count; ++p) {
			free(target->types.items[p].key);
			free(target->types.items[p].value);
		}

		free(target->types.items);
	}

	free(config->targets.items);

	for (size_t i = 0; i < config->types_by_name.count; ++i) {
		free(config->types_by_name.items[i].key);
		free(config->types_by_name.items[i].value);
	}

	free(config->types_by_name.items);

	for (size_t i = 0; i < config->types_by_declared.count; ++i) {
		free(config->types_by_declared.items[i].key);
		free(config->types_by_declared.items[i].value);
	}

	free(config->types_by_declared.items);

	for (size_t i = 0; i < config->tables.count; ++i) {
		free(config->tables.items[i].name);
	}

	free(config->tables.items);

	for (size_t i = 0; i < config->rules.count; ++i) {
		sg_rule_config *rule = &config->rules.items[i];

		for (size_t p = 0; p < rule->params.count; ++p) {
			free(rule->params.items[p].key);
			free(rule->params.items[p].value);
		}

		free(rule->params.items);
	}

	free(config->rules.items);

	for (size_t i = 0; i < config->queries.count; ++i) {
		free(config->queries.items[i]);
	}

	free(config->queries.items);
	memset(config, 0, sizeof *config);
}

static void apply(sg_table_options *into, const sg_table_options *from) {
	if (from->has_writes) {
		into->has_writes = true;
		into->writes     = from->writes;
	}
}

sg_table_options sg_config_table(const sg_config *config, const char *table) {
	sg_table_options options = { false, 0 };

	for (size_t i = 0; i < config->tables.count; ++i) {
		if (strcmp(config->tables.items[i].name, "*") == 0) {
			apply(&options, &config->tables.items[i].options);
		}
	}

	for (size_t i = 0; i < config->tables.count; ++i) {
		if (sg_strcasecmp(config->tables.items[i].name, table) == 0) {
			apply(&options, &config->tables.items[i].options);
		}
	}

	return options;
}

const sg_rule_config *sg_config_rule(const sg_config *config, const char *name) {
	for (size_t i = 0; i < config->rules.count; ++i) {
		if (strcmp(config->rules.items[i].name, name) == 0) {
			return &config->rules.items[i];
		}
	}

	return NULL;
}

const char *sg_rule_param(const sg_rule_config *rule, const char *name) {
	for (size_t i = 0; i < rule->params.count; ++i) {
		if (strcmp(rule->params.items[i].key, name) == 0) {
			return rule->params.items[i].value;
		}
	}

	return NULL;
}
