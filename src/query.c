#include "query.h"

#include <glob.h>
#include <stdlib.h>
#include <string.h>

#include "plan.h"

typedef struct {
	char *name;
	char *type;
	bool  nullable;
	bool  used;
	unsigned line;
} annotation;

typedef SG_VEC(annotation) annotations;

typedef struct {
	const char   *path;
	annotations   params;
	annotations   columns;
	int           returns;     /* -1 unset, else an sg_rows */
	bool          allow_scan;
} notes;

static void emit_word(const char *word, void *user) {
	*SG_PUSH(*(sg_names *)user) = sg_xstrdup(word);
}

static void words_of(const char *name, sg_names *out) {
	sg_split_words(name, emit_word, out);
}

static bool type_name(const char *type, sg_affinity *out) {
	static const struct {
		const char *name;
		sg_affinity affinity;
	} TYPES[] = {
		{ "integer", SG_AFF_INTEGER }, { "real", SG_AFF_REAL }, { "text", SG_AFF_TEXT },
		{ "blob", SG_AFF_BLOB }, { "numeric", SG_AFF_NUMERIC }, { NULL, SG_AFF_BLOB }
	};

	for (size_t i = 0; TYPES[i].name; ++i) {
		if (strcmp(TYPES[i].name, type) == 0) {
			*out = TYPES[i].affinity;
			return true;
		}
	}

	return false;
}

/* "-- @key rest" lines anywhere in the file. */
static bool read_notes(const char *path, const char *text, notes *out) {
	bool     ok   = true;
	unsigned line = 0;

	out->returns = -1;

	for (const char *at = text; *at; ) {
		const char *end = strchr(at, '\n');
		size_t      len = end ? (size_t)(end - at) : strlen(at);
		const char *c   = at;

		++line;

		while (c < at + len && (*c == ' ' || *c == '\t')) {
			++c;
		}

		if (len - (size_t)(c - at) >= 3 && strncmp(c, "--", 2) == 0) {
			const char *d = c + 2;

			while (d < at + len && (*d == ' ' || *d == '\t')) {
				++d;
			}

			if (d < at + len && *d == '@') {
				char  *note  = sg_xstrndup(d + 1, (size_t)(at + len - d - 1));
				char  *words[4] = { NULL, NULL, NULL, NULL };
				int    count    = 0;
				unsigned col    = (unsigned)(d - at) + 1;

				for (char *w = strtok(note, " \t\r"); w && count < 4; w = strtok(NULL, " \t\r")) {
					words[count++] = w;
				}

				sg_affinity affinity;

				if (count >= 3 && (strcmp(words[0], "param") == 0 || strcmp(words[0], "column") == 0)) {
					bool nullable = count == 4 && strcmp(words[3], "null") == 0;

					if (!type_name(words[2], &affinity) || (count == 4 && !nullable)) {
						sg_error(path, line, col, "@%s %s takes a type -- integer, real, text, blob or"
								" numeric -- and then optionally null", words[0], words[1]);
						ok = false;
					} else {
						annotations *list = words[0][0] == 'p' ? &out->params : &out->columns;
						annotation  *a    = SG_PUSH(*list);

						a->name     = sg_xstrdup(words[1][0] == ':' || words[1][0] == '@' || words[1][0] == '$'
								? words[1] + 1 : words[1]);
						a->type     = sg_xstrdup(words[2]);
						a->nullable = nullable;
						a->line     = line;
					}
				} else if (count == 2 && strcmp(words[0], "returns") == 0
						&& (strcmp(words[1], "one") == 0 || strcmp(words[1], "many") == 0)) {
					out->returns = words[1][0] == 'o' ? SG_ROWS_AT_MOST_ONE : SG_ROWS_MANY;
				} else if (count == 2 && strcmp(words[0], "allow") == 0 && strcmp(words[1], "scan") == 0) {
					out->allow_scan = true;
				} else {
					sg_error(path, line, col, "not an annotation sqlgen knows; known: @param name type [null],"
							" @column name type [null], @returns one|many, @allow scan");
					ok = false;
				}

				free(note);
			}
		}

		at = end ? end + 1 : at + len;
	}

	return ok;
}

static void free_notes(notes *n) {
	for (size_t i = 0; i < n->params.count; ++i) {
		free(n->params.items[i].name);
		free(n->params.items[i].type);
	}

	for (size_t i = 0; i < n->columns.count; ++i) {
		free(n->columns.items[i].name);
		free(n->columns.items[i].type);
	}

	free(n->params.items);
	free(n->columns.items);
}

static annotation *note_for(annotations *list, const char *name) {
	for (size_t i = 0; i < list->count; ++i) {
		if (strcmp(list->items[i].name, name) == 0) {
			list->items[i].used = true;
			return &list->items[i];
		}
	}

	return NULL;
}

/* The affinity the analysis's storage classes amount to; false when they amount to nothing one. */
static bool affinity_of(unsigned types, sg_affinity *out) {
	unsigned t = types & ~(unsigned)SG_BT_NULL;

	if (t & SG_BT_UNKNOWN || t == 0) {
		return false;
	}

	if (t == SG_BT_INT) {
		*out = SG_AFF_INTEGER;
	} else if (t == SG_BT_REAL) {
		*out = SG_AFF_REAL;
	} else if (t == SG_BT_TEXT) {
		*out = SG_AFF_TEXT;
	} else if (t == SG_BT_BLOB) {
		*out = SG_AFF_BLOB;
	} else if (t == (SG_BT_INT | SG_BT_REAL)) {
		*out = SG_AFF_NUMERIC;
	} else {
		return false;
	}

	return true;
}

/* A field from one table column: that column's logical type and typed id. */
static void from_column(const sg_config *config, const sg_model *model, sg_bsource source,
		sg_query_field *f) {
	const sg_column *column = &source.table->columns.items[source.column];
	bool             custom = false;

	f->type     = sg_xstrdup(sg_plan_logical_type(config, column, &custom));
	f->custom   = custom;
	f->affinity = column->affinity;
	f->id_of    = config->typed_ids ? sg_plan_id_of(model, source.table, column) : NULL;
}

static void from_affinity(sg_affinity affinity, sg_query_field *f) {
	f->type     = sg_xstrdup(sg_affinity_name(affinity));
	f->affinity = affinity;
}

static bool type_params(sqlite3_stmt *stmt, const sg_config *config, const sg_model *model,
		const sg_banalysis *analysis, notes *n, const char *path, unsigned line, sg_query *q) {
	bool ok = true;

	for (int p = 1; p <= sqlite3_bind_parameter_count(stmt); ++p) {
		const char      *raw    = sqlite3_bind_parameter_name(stmt, p);
		const sg_bparam *info   = (size_t)p <= analysis->params.count ? &analysis->params.items[p - 1] : NULL;
		bool             single = info && info->source.table && info->source.column >= 0 && !info->several;
		sg_query_field  *f      = SG_PUSH(q->params);

		f->index = p;

		if (raw && raw[0] != '?') {
			f->name = sg_xstrdup(raw + 1);
		} else if (single) {
			f->name = sg_xstrdup(info->source.table->columns.items[info->source.column].name);
		} else {
			sg_error(path, line, 0, "parameter %d has no name, and no single column it is compared with or"
					" stored into names it; write it as :name", p);
			--q->params.count;
			ok = false;
			continue;
		}

		words_of(f->name, &f->words);

		for (size_t i = 0; i + 1 < q->params.count; ++i) {
			if (strcmp(q->params.items[i].name, f->name) == 0) {
				sg_error(path, line, 0, "two parameters are called %s; name the positional one", f->name);
				ok = false;
			}
		}

		annotation *note = note_for(&n->params, f->name);
		sg_affinity affinity = SG_AFF_BLOB;

		if (note) {
			type_name(note->type, &affinity);
			from_affinity(affinity, f);
			f->nullable = note->nullable;
		} else if (single) {
			from_column(config, model, info->source, f);

			/* Written into a nullable column, a parameter may be NULL; compared, NULL matches nothing. */
			f->nullable = q->writes && !info->source.table->columns.items[info->source.column].notnull;
		} else if (info && affinity_of(info->types, &affinity)) {
			from_affinity(affinity, f);
		} else {
			sg_error(path, line, 0, "the type of :%s cannot be told from the statement; add a line"
					" -- @param %s <type>", f->name, f->name);
			ok = false;
		}
	}

	return ok;
}

static bool type_columns(sqlite3_stmt *stmt, const sg_config *config, const sg_model *model,
		const sg_banalysis *analysis, notes *n, const char *path, unsigned line, sg_query *q) {
	bool ok = true;

	for (int c = 0; c < sqlite3_column_count(stmt); ++c) {
		const sg_bresult *info = (size_t)c < analysis->results.count ? &analysis->results.items[c] : NULL;
		sg_query_field   *f    = SG_PUSH(q->columns);
		annotation       *note;
		sg_affinity       affinity = SG_AFF_BLOB;

		f->name = sg_xstrdup(sqlite3_column_name(stmt, c));
		words_of(f->name, &f->words);

		for (size_t i = 0; i + 1 < q->columns.count; ++i) {
			if (sg_strcasecmp(q->columns.items[i].name, f->name) == 0) {
				sg_error(path, line, 0, "two result columns are called %s; give one an alias (as …)", f->name);
				ok = false;
			}
		}

		if ((note = note_for(&n->columns, f->name)) != NULL) {
			type_name(note->type, &affinity);
			from_affinity(affinity, f);
			f->nullable = note->nullable;
		} else if (info && info->source.table && info->source.column >= 0) {
			from_column(config, model, info->source, f);
			f->nullable = (info->types & SG_BT_NULL) != 0;
		} else if (info && affinity_of(info->types, &affinity)) {
			from_affinity(affinity, f);
			f->nullable = (info->types & SG_BT_NULL) != 0;
		} else {
			sg_error(path, line, 0, "the type of the result column %s cannot be told from the statement;"
					" add a line -- @column %s <type> [null]", f->name, f->name);
			ok = false;
		}
	}

	return ok;
}

/* A query selecting exactly one table's columns, in order, under their names. */
static const sg_table *whole_row(const sg_banalysis *analysis, const sg_query *q) {
	const sg_table *table = analysis->results.count ? analysis->results.items[0].source.table : NULL;

	if (!table || table->kind == SG_VIRTUAL || analysis->results.count != table->columns.count) {
		return NULL;
	}

	for (size_t c = 0; c < table->columns.count; ++c) {
		const sg_bresult *r = &analysis->results.items[c];

		if (r->source.table != table || r->source.column != (int)c
				|| strcmp(q->columns.items[c].name, table->columns.items[c].name) != 0
				|| q->columns.items[c].nullable != !table->columns.items[c].notnull) {
			return NULL;
		}
	}

	return table;
}

static void find_scans(sqlite3 *db, const char *sql, sg_names *out) {
	char         *plan = sqlite3_mprintf("explain query plan %s", sql);
	sqlite3_stmt *stmt = NULL;

	if (sqlite3_prepare_v2(db, plan, -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			const char *detail = (const char *)sqlite3_column_text(stmt, 3);

			if (detail && strncmp(detail, "SCAN ", 5) == 0 && strncmp(detail, "SCAN CONSTANT", 13) != 0) {
				*SG_PUSH(*out) = sg_xstrdup(detail + 5);
			}
		}
	}

	sqlite3_finalize(stmt);
	sqlite3_free(plan);
}

/* Past whitespace and comments. */
static const char *skip_blank(const char *s) {
	while (*s) {
		if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
			++s;
		} else if (s[0] == '-' && s[1] == '-') {
			s += strcspn(s, "\n");
		} else if (s[0] == '/' && s[1] == '*') {
			const char *end = strstr(s + 2, "*/");

			s = end ? end + 2 : s + strlen(s);
		} else {
			break;
		}
	}

	return s;
}

static bool nothing_left(const char *s) {
	return *skip_blank(s) == '\0';
}

/* The `--` lines before the statement that are no annotation, without their dashes: what the
 * query is for, in the author's words. */
static void leading_doc(const char *text, const char *start, sg_names *out) {
	for (const char *s = text; s < start; ) {
		size_t      n    = strcspn(s, "\n");
		const char *line = s;

		while (line < s + n && (*line == ' ' || *line == '\t')) {
			++line;
		}

		if (line + 1 < s + n && line[0] == '-' && line[1] == '-') {
			const char *body = line + 2;
			size_t      blen = (size_t)(s + n - body);

			while (blen && (*body == ' ' || *body == '\t')) {
				++body;
				--blen;
			}

			while (blen && strchr(" \t\r", body[blen - 1])) {
				--blen;
			}

			if (*body != '@') {
				*SG_PUSH(*out) = sg_xstrndup(body, blen);
			}
		}

		s += n + (s[n] == '\n');
	}
}

static char *base_name(const char *path) {
	const char *slash = strrchr(path, '/');
	const char *base  = slash ? slash + 1 : path;
	size_t      len   = strlen(base);

	return sg_xstrndup(base, len > 4 && strcmp(base + len - 4, ".sql") == 0 ? len - 4 : len);
}

static bool load_query(sqlite3 *db, const sg_model *model, const sg_config *config, const char *path,
		sg_query *q) {
	char         *text = NULL;
	size_t        len  = 0;
	notes         n;
	sqlite3_stmt *stmt = NULL;
	const char   *tail = NULL;
	bool          ok;

	memset(&n, 0, sizeof n);
	q->path = sg_xstrdup(path);
	q->name = base_name(path);
	words_of(q->name, &q->words);

	if (!sg_read_file(path, &text, &len)) {
		return false;
	}

	ok = read_notes(path, text, &n);

	const char *start = skip_blank(text);

	sg_line_col(text, (size_t)(start - text), &q->line, &q->col);

	if (q->words.count == 0) {
		sg_error(path, 0, 0, "the file name gives the query no name");
		ok = false;
	}

	if (sqlite3_prepare_v2(db, text, (int)len, &stmt, &tail) != SQLITE_OK) {
		int      offset = sqlite3_error_offset(db);
		unsigned line;
		unsigned col;

		sg_line_col(text, offset >= 0 ? (size_t)offset : (size_t)(start - text), &line, &col);
		sg_error(path, line, col, "%s", sqlite3_errmsg(db));
		free(text);
		free_notes(&n);
		return false;
	}

	if (!stmt) {
		sg_error(path, 0, 0, "holds no statement");
		free(text);
		free_notes(&n);
		return false;
	}

	if (tail && !nothing_left(tail)) {
		unsigned line;
		unsigned col;

		sg_line_col(text, (size_t)(tail - text), &line, &col);
		sg_error(path, line, col, "a second statement; a query file holds exactly one");
		ok = false;
	}

	size_t end = tail ? (size_t)(tail - text) : len;

	while (end > (size_t)(start - text) && strchr(" \t\r\n;", text[end - 1])) {
		--end;
	}

	q->sql    = sg_xstrndup(start, end - (size_t)(start - text));
	leading_doc(text, start, &q->doc);
	q->writes = !sqlite3_stmt_readonly(stmt);

	sg_banalysis analysis;

	if (ok && sg_bytecode_analyze(db, model, q->sql, path, &analysis)) {
		ok = type_params(stmt, config, model, &analysis, &n, path, q->line, q) && ok;
		ok = type_columns(stmt, config, model, &analysis, &n, path, q->line, q) && ok;
		q->rows       = n.returns >= 0 ? (sg_rows)n.returns : analysis.rows;
		q->row_table  = ok ? whole_row(&analysis, q) : NULL;
		q->allow_scan = n.allow_scan;

		if (q->columns.count == 0) {
			q->rows = SG_ROWS_NONE;
		}

		sg_banalysis_free(&analysis);
	} else {
		ok = false;
	}

	for (size_t i = 0; i < n.params.count; ++i) {
		if (!n.params.items[i].used) {
			sg_error(path, n.params.items[i].line, 0, "@param %s names no parameter of the statement",
					n.params.items[i].name);
			ok = false;
		}
	}

	for (size_t i = 0; i < n.columns.count; ++i) {
		if (!n.columns.items[i].used) {
			sg_error(path, n.columns.items[i].line, 0, "@column %s names no result column",
					n.columns.items[i].name);
			ok = false;
		}
	}

	find_scans(db, q->sql, &q->scans);
	sqlite3_finalize(stmt);
	free(text);
	free_notes(&n);
	return ok;
}

static int compare_paths(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

bool sg_queries_load(sqlite3 *db, const sg_model *model, const sg_config *config, sg_queries *out) {
	sg_str_vec paths = {0};
	bool       ok    = true;

	memset(out, 0, sizeof *out);

	for (size_t i = 0; i < config->queries.count; ++i) {
		glob_t found = {0};
		int    rc    = glob(config->queries.items[i], 0, NULL, &found);

		if (rc == GLOB_NOMATCH) {
			sg_error(config->path, 0, 0, "$.queries: '%s' matches nothing", config->queries.items[i]);
			ok = false;
		}

		for (size_t j = 0; rc == 0 && j < found.gl_pathc; ++j) {
			if (sg_is_file(found.gl_pathv[j])) {
				*SG_PUSH(paths) = sg_xstrdup(found.gl_pathv[j]);
			}
		}

		globfree(&found);
	}

	if (paths.count) {
		qsort(paths.items, paths.count, sizeof *paths.items, compare_paths);
	}

	for (size_t i = 0; i < paths.count; ++i) {
		if (i && strcmp(paths.items[i], paths.items[i - 1]) == 0) {
			continue;
		}

		sg_query *q = SG_PUSH(*out);

		ok = load_query(db, model, config, paths.items[i], q) && ok;

		for (size_t j = 0; j + 1 < out->count; ++j) {
			if (strcmp(out->items[j].name, q->name) == 0) {
				sg_error(q->path, 0, 0, "is a second query named %s, after %s", q->name, out->items[j].path);
				ok = false;
			}
		}
	}

	for (size_t i = 0; i < paths.count; ++i) {
		free(paths.items[i]);
	}

	free(paths.items);
	return ok;
}

static void free_fields(sg_query_fields *fields) {
	for (size_t i = 0; i < fields->count; ++i) {
		free(fields->items[i].name);
		free(fields->items[i].type);

		for (size_t w = 0; w < fields->items[i].words.count; ++w) {
			free(fields->items[i].words.items[w]);
		}

		free(fields->items[i].words.items);
	}

	free(fields->items);
}

void sg_queries_free(sg_queries *queries) {
	for (size_t i = 0; i < queries->count; ++i) {
		sg_query *q = &queries->items[i];

		free(q->name);
		free(q->path);
		free(q->sql);
		free_fields(&q->params);
		free_fields(&q->columns);

		for (size_t w = 0; w < q->words.count; ++w) {
			free(q->words.items[w]);
		}

		for (size_t s = 0; s < q->scans.count; ++s) {
			free(q->scans.items[s]);
		}

		for (size_t d = 0; d < q->doc.count; ++d) {
			free(q->doc.items[d]);
		}

		free(q->words.items);
		free(q->scans.items);
		free(q->doc.items);
	}

	free(queries->items);
	memset(queries, 0, sizeof *queries);
}
