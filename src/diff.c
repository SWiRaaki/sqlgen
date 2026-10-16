#include "diff.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "jsonw.h"

/* ---- following tables through the steps ---------------------------------------------------- */

/* A column of a table that existed at the start, under its name at the start and its name now. */
typedef struct {
	const char *original;
	char       *current;
	bool        renamed;
	sg_origin   renamed_at;
} column_line;

/* One table through the steps: what it was called at the start (NULL when created since), what it
 * is called now, and the statements that created, last reshaped and last renamed it. */
typedef struct {
	const char            *original;
	char                  *current;
	bool                   alive;
	bool                   created;
	sg_origin              created_at;
	bool                   shaped;
	sg_origin              shaped_at;
	bool                   renamed;
	sg_origin              renamed_at;
	SG_VEC(column_line)    columns;
} lineage;

/* A table of the start dropped on the way, or an index or trigger last touched by a statement. */
typedef struct {
	const char *type;
	char       *name;
	sg_origin   at;
} mark;

typedef SG_VEC(mark) marks;

typedef struct {
	SG_VEC(lineage) lines;
	marks           dropped;   //!< type "table"; name as at the start.
	marks           touched;   //!< type "index" or "trigger".
} history;

static lineage *line_named(history *h, const char *current) {
	for (size_t i = 0; i < h->lines.count; ++i) {
		if (h->lines.items[i].alive && sg_strcasecmp(h->lines.items[i].current, current) == 0) {
			return &h->lines.items[i];
		}
	}

	return NULL;
}

static void set_mark(marks *list, const char *type, const char *name, sg_origin at) {
	for (size_t i = 0; i < list->count; ++i) {
		if (strcmp(list->items[i].type, type) == 0 && sg_strcasecmp(list->items[i].name, name) == 0) {
			list->items[i].at = at;
			return;
		}
	}

	mark *m = SG_PUSH(*list);

	m->type = type;
	m->name = sg_xstrdup(name);
	m->at   = at;
}

static const mark *find_mark(const marks *list, const char *type, const char *name) {
	for (size_t i = 0; i < list->count; ++i) {
		if (strcmp(list->items[i].type, type) == 0 && sg_strcasecmp(list->items[i].name, name) == 0) {
			return &list->items[i];
		}
	}

	return NULL;
}

/* ---- describing ---------------------------------------------------------------------------- */

/* A statement with whitespace runs made one space, none beside punctuation, and letters lowercase
 * outside quotes: two spellings of the same definition compare equal. */
static char *normalized(const char *sql) {
	sg_buf out   = {0};
	char   quote = 0;
	bool   space = false;

	for (const char *s = sql ? sql : ""; *s; ++s) {
		char c = *s;

		if (quote) {
			sg_buf_putc(&out, c);
			quote = c == quote ? 0 : quote;
			continue;
		}

		if (isspace((unsigned char)c)) {
			space = true;
			continue;
		}

		bool punct = strchr("(),;", c) != NULL;
		char last  = out.len ? out.data[out.len - 1] : '(';

		/* One space after a comma, none inside parentheses or before punctuation. */
		if ((last == ',' && c != ')') || (space && !punct && last != '(' && last != ',')) {
			sg_buf_putc(&out, ' ');
		}

		space = false;

		if (c == '\'' || c == '"' || c == '`' || c == '[') {
			quote = c == '[' ? ']' : c;
		}

		sg_buf_putc(&out, (char)tolower((unsigned char)c));
	}

	while (out.len && out.data[out.len - 1] == ';') {
		out.data[--out.len] = '\0';
	}

	return out.data ? out.data : sg_xstrdup("");
}

static const char *kind_word(const sg_table *t) {
	return t->kind == SG_VIEW ? "view" : t->kind == SG_VIRTUAL ? "virtual table" : "table";
}

static char *describe_flags(const sg_table *t) {
	sg_buf out = {0};

	sg_buf_puts(&out, kind_word(t));

	if (t->without_rowid) {
		sg_buf_puts(&out, ", without rowid");
	}

	if (t->strict) {
		sg_buf_puts(&out, ", strict");
	}

	if (t->autoincrement) {
		sg_buf_puts(&out, ", autoincrement");
	}

	return out.data;
}

static char *describe_column(const sg_column *c) {
	sg_buf out = {0};

	sg_buf_puts(&out, c->declared_type[0] ? c->declared_type : "(no type)");

	for (char *ch = out.data; *ch; ++ch) {
		*ch = (char)tolower((unsigned char)*ch);
	}

	if (c->notnull_declared) {
		sg_buf_puts(&out, " not null");
	}

	if (c->default_sql) {
		sg_buf_printf(&out, " default %s", c->default_sql);
	}

	if (c->collation && sg_strcasecmp(c->collation, "binary") != 0) {
		sg_buf_printf(&out, " collate %s", c->collation);
	}

	if (c->generated != SG_GEN_NONE) {
		sg_buf_puts(&out, c->generated == SG_GEN_STORED ? " generated stored" : " generated virtual");
	}

	if (c->hidden) {
		sg_buf_puts(&out, " hidden");
	}

	return out.data;
}

/* A table's columns and keys, with one column optionally spelt under another name, so that a step
 * renaming that column compares equal to the one before. */
static char *describe_shape(const sg_table *t, const char *from, const char *to) {
	sg_buf out = {0};
	char  *flags = describe_flags(t);

	sg_buf_puts(&out, flags);
	free(flags);

	for (size_t i = 0; i < t->columns.count; ++i) {
		const sg_column *c    = &t->columns.items[i];
		char            *desc = describe_column(c);
		const char      *name = from && sg_strcasecmp(c->name, from) == 0 ? to : c->name;

		sg_buf_printf(&out, "\n%s %s %d", name, desc, c->pk);
		free(desc);
	}

	for (size_t i = 0; i < t->foreign_keys.count; ++i) {
		const sg_foreign_key *fk = &t->foreign_keys.items[i];

		sg_buf_printf(&out, "\nfk %s", fk->parent);

		for (size_t j = 0; j < fk->columns.count; ++j) {
			const char *name = from && sg_strcasecmp(fk->columns.items[j], from) == 0 ? to : fk->columns.items[j];

			sg_buf_printf(&out, " %s", name);
		}

		sg_buf_printf(&out, " %s %s", fk->on_update ? fk->on_update : "", fk->on_delete ? fk->on_delete : "");
	}

	return out.data;
}

static bool same_shape(const sg_table *a, const sg_table *b, const char *from, const char *to) {
	char *x  = describe_shape(a, from, to);
	char *y  = describe_shape(b, NULL, NULL);
	bool  eq = strcmp(x, y) == 0;

	free(x);
	free(y);
	return eq;
}

/* Two tables of one step with the same columns, ignoring the table's own name. */
static bool same_columns(const sg_table *a, const sg_table *b) {
	return same_shape(a, b, NULL, NULL);
}

/* ---- one step ----------------------------------------------------------------------------- */

static bool in_model(const sg_model *m, const char *name) {
	return sg_model_table(m, name) != NULL;
}

/* A column renamed in this step: exactly one name gone and one new, at the same position, with the
 * same declaration. */
static bool column_rename(const sg_table *before, const sg_table *after, const sg_column **from,
		const sg_column **to) {
	if (before->columns.count != after->columns.count) {
		return false;
	}

	size_t changed = 0;

	for (size_t i = 0; i < before->columns.count; ++i) {
		const sg_column *b = &before->columns.items[i];
		const sg_column *a = &after->columns.items[i];

		if (sg_strcasecmp(b->name, a->name) != 0) {
			char *x = describe_column(b);
			char *y = describe_column(a);
			bool  eq = strcmp(x, y) == 0 && !sg_table_column(after, b->name) && !sg_table_column(before, a->name);

			free(x);
			free(y);

			if (!eq) {
				return false;
			}

			*from = b;
			*to   = a;
			++changed;
		}
	}

	return changed == 1;
}

static void step(history *h, const sg_model *prev, const sg_model *cur, sg_origin at, bool renames) {
	const sg_table *gone    = NULL;
	const sg_table *came    = NULL;
	size_t          ngone   = 0;
	size_t          ncame   = 0;

	for (size_t i = 0; i < prev->tables.count; ++i) {
		if (!in_model(cur, prev->tables.items[i].name)) {
			gone = &prev->tables.items[i];
			++ngone;
		}
	}

	for (size_t i = 0; i < cur->tables.count; ++i) {
		if (!in_model(prev, cur->tables.items[i].name)) {
			came = &cur->tables.items[i];
			++ncame;
		}
	}

	if (renames && ngone == 1 && ncame == 1 && gone->kind == came->kind && same_columns(gone, came)) {
		lineage *line = line_named(h, gone->name);

		if (line) {
			free(line->current);
			line->current    = sg_xstrdup(came->name);
			line->renamed    = true;
			line->renamed_at = at;
		}
	} else {
		for (size_t i = 0; i < prev->tables.count; ++i) {
			const sg_table *t = &prev->tables.items[i];

			if (!in_model(cur, t->name)) {
				lineage *line = line_named(h, t->name);

				if (line && line->original) {
					set_mark(&h->dropped, "table", line->original, at);
				}

				if (line) {
					line->alive = false;
				}
			}
		}

		for (size_t i = 0; i < cur->tables.count; ++i) {
			const sg_table *t = &cur->tables.items[i];

			if (!in_model(prev, t->name)) {
				lineage *line = SG_PUSH(h->lines);

				line->current    = sg_xstrdup(t->name);
				line->alive      = true;
				line->created    = true;
				line->created_at = at;
				line->shaped     = true;
				line->shaped_at  = at;
			}
		}
	}

	/* Tables in both: reshaped, or a column renamed. */
	for (size_t i = 0; i < cur->tables.count; ++i) {
		const sg_table *after  = &cur->tables.items[i];
		const sg_table *before = sg_model_table(prev, after->name);
		lineage        *line   = line_named(h, after->name);
		const sg_column *from  = NULL;
		const sg_column *to    = NULL;

		if (!before || !line) {
			continue;
		}

		if (renames && column_rename(before, after, &from, &to) && same_shape(before, after, from->name, to->name)) {
			for (size_t c = 0; c < line->columns.count; ++c) {
				column_line *col = &line->columns.items[c];

				if (sg_strcasecmp(col->current, from->name) == 0) {
					free(col->current);
					col->current    = sg_xstrdup(to->name);
					col->renamed    = true;
					col->renamed_at = at;
				}
			}
		} else if (!same_shape(before, after, NULL, NULL)) {
			line->shaped    = true;
			line->shaped_at = at;
		}
	}

	/* Indexes and triggers: which statement last made, dropped or changed each. */
	static const char *const TYPES[] = { "index", "trigger", "view", "table" };

	for (size_t t = 0; t < 4; ++t) {
		for (size_t i = 0; i < cur->objects.count; ++i) {
			const sg_object *o = &cur->objects.items[i];
			const sg_object *p = strcmp(o->type, TYPES[t]) == 0 ? sg_model_object(prev, o->type, o->name) : NULL;

			if (strcmp(o->type, TYPES[t]) == 0 && (!p || strcmp(p->sql, o->sql) != 0)) {
				if (t < 2) {
					set_mark(&h->touched, TYPES[t], o->name, at);
				} else if (p) {
					/* A view's or virtual table's statement is its shape. */
					lineage *line = line_named(h, o->name);

					if (line && t == 2) {
						line->shaped    = true;
						line->shaped_at = at;
					}
				}
			}
		}

		for (size_t i = 0; t < 2 && i < prev->objects.count; ++i) {
			const sg_object *o = &prev->objects.items[i];

			if (strcmp(o->type, TYPES[t]) == 0 && !sg_model_object(cur, o->type, o->name)) {
				set_mark(&h->touched, TYPES[t], o->name, at);
			}
		}
	}
}

/* ---- comparing the two ends ---------------------------------------------------------------- */

typedef struct {
	const sg_table *old_table;
	const sg_table *new_table;
	lineage        *line;      //!< NULL without steps.
	SG_VEC(int)     map;       //!< Per old column: the index of its new column, or -1.
} pair;

typedef SG_VEC(pair) pairs;

typedef struct {
	sg_changes *out;
	const pairs *pairs;
	bool        located;
} emitter;

static sg_change *add(emitter *e, sg_change_kind kind, sg_thing thing, const char *table, const char *name) {
	sg_change *c = SG_PUSH(*e->out);

	c->kind  = kind;
	c->thing = thing;
	c->table = sg_xstrdup(table);
	c->name  = name ? sg_xstrdup(name) : NULL;
	return c;
}

static void locate(sg_change *c, bool located, sg_origin at) {
	c->located = located;
	c->at      = at;
}

static void locate_shape(emitter *e, sg_change *c, const pair *p) {
	if (e->located && p->line && p->line->shaped) {
		locate(c, true, p->line->shaped_at);
	}
}

/* The new name of an old table's column, through the pairs; NULL when it has none. */
static const char *mapped_column(const pairs *ps, const char *table, const char *column) {
	for (size_t i = 0; i < ps->count; ++i) {
		const pair *p = &ps->items[i];

		if (sg_strcasecmp(p->old_table->name, table) != 0) {
			continue;
		}

		for (size_t c = 0; c < p->old_table->columns.count; ++c) {
			if (sg_strcasecmp(p->old_table->columns.items[c].name, column) == 0) {
				return p->map.items[c] >= 0 ? p->new_table->columns.items[p->map.items[c]].name : NULL;
			}
		}
	}

	return NULL;
}

static const char *mapped_table(const pairs *ps, const char *table) {
	for (size_t i = 0; i < ps->count; ++i) {
		if (sg_strcasecmp(ps->items[i].old_table->name, table) == 0) {
			return ps->items[i].new_table->name;
		}
	}

	return table;
}

/* A list of names, each mapped to its new spelling when @p table is given. */
static void put_names(sg_buf *out, const pairs *ps, const char *table, const sg_names *names) {
	sg_buf_putc(out, '(');

	for (size_t i = 0; i < names->count; ++i) {
		const char *name   = names->items[i];
		const char *mapped = table && name[0] ? mapped_column(ps, table, name) : NULL;

		sg_buf_printf(out, "%s%s", i ? ", " : "", mapped ? mapped : name[0] ? name : "<expression>");
	}

	sg_buf_putc(out, ')');
}

static bool is_primary(const sg_table *t, const sg_unique_key *key) {
	if (strcmp(key->source, "rowid") == 0) {
		return true;
	}

	for (size_t i = 0; i < t->indexes.count; ++i) {
		if (strcmp(t->indexes.items[i].name, key->source) == 0) {
			return strcmp(t->indexes.items[i].origin, "primary_key") == 0;
		}
	}

	return false;
}

static bool is_constraint(const sg_table *t, const sg_unique_key *key) {
	if (is_primary(t, key)) {
		return true;
	}

	for (size_t i = 0; i < t->indexes.count; ++i) {
		if (strcmp(t->indexes.items[i].name, key->source) == 0) {
			return strcmp(t->indexes.items[i].origin, "unique") == 0;
		}
	}

	return false;
}

static char *describe_key(const pairs *ps, const sg_table *t, const sg_unique_key *key, bool old) {
	sg_buf out = {0};

	sg_buf_puts(&out, is_primary(t, key) ? "primary key " : "unique ");
	put_names(&out, ps, old ? t->name : NULL, &key->columns);
	return out.data;
}

static char *describe_fk(const pairs *ps, const sg_table *t, const sg_foreign_key *fk, bool old) {
	sg_buf      out    = {0};
	const char *parent = old ? mapped_table(ps, fk->parent) : fk->parent;

	put_names(&out, ps, old ? t->name : NULL, &fk->columns);
	sg_buf_printf(&out, " references %s", parent);
	put_names(&out, ps, old ? fk->parent : NULL, &fk->resolved);

	if (fk->on_update && sg_strcasecmp(fk->on_update, "no action") != 0) {
		sg_buf_printf(&out, " on update %s", fk->on_update);
	}

	if (fk->on_delete && sg_strcasecmp(fk->on_delete, "no action") != 0) {
		sg_buf_printf(&out, " on delete %s", fk->on_delete);
	}

	for (char *c = out.data; c && *c; ++c) {
		*c = (char)tolower((unsigned char)*c);
	}

	return out.data;
}

static char *describe_index(const pairs *ps, const sg_model *m, const sg_table *t, const sg_index *ix,
		bool old, bool by_sql) {
	if (by_sql) {
		const sg_object *o = sg_model_object(m, "index", ix->name);

		return normalized(o ? o->sql : "");
	}

	sg_buf out = {0};

	sg_buf_puts(&out, ix->unique ? "unique on " : "on ");

	sg_buf_putc(&out, '(');

	for (size_t i = 0; i < ix->columns.count; ++i) {
		const char *name   = ix->columns.items[i];
		const char *mapped = old && name[0] ? mapped_column(ps, t->name, name) : NULL;

		sg_buf_printf(&out, "%s%s", i ? ", " : "", mapped ? mapped : name[0] ? name : "<expression>");

		if (i < ix->descending.count && ix->descending.items[i]) {
			sg_buf_puts(&out, " desc");
		}

		if (i < ix->collations.count && sg_strcasecmp(ix->collations.items[i], "binary") != 0) {
			sg_buf_printf(&out, " collate %s", ix->collations.items[i]);
		}
	}

	sg_buf_putc(&out, ')');

	if (ix->partial) {
		sg_buf_puts(&out, " where …");
	}

	return out.data;
}

typedef SG_VEC(char *) texts;

static void free_texts(texts *t) {
	for (size_t i = 0; i < t->count; ++i) {
		free(t->items[i]);
	}

	free(t->items);
	memset(t, 0, sizeof *t);
}

static bool has_text(const texts *t, const char *text) {
	for (size_t i = 0; i < t->count; ++i) {
		if (strcmp(t->items[i], text) == 0) {
			return true;
		}
	}

	return false;
}

/* Two sets of descriptions: what only the old has is removed, what only the new has is added. */
static void compare_sets(emitter *e, const pair *p, sg_thing thing, const texts *olds, const texts *news) {
	for (size_t i = 0; i < olds->count; ++i) {
		if (!has_text(news, olds->items[i])) {
			sg_change *c = add(e, SG_CHANGE_REMOVE, thing, p->new_table->name, NULL);

			c->before    = sg_xstrdup(olds->items[i]);
			c->old_table = p->old_table;
			c->new_table = p->new_table;
			locate_shape(e, c, p);
		}
	}

	for (size_t i = 0; i < news->count; ++i) {
		if (!has_text(olds, news->items[i])) {
			sg_change *c = add(e, SG_CHANGE_ADD, thing, p->new_table->name, NULL);

			c->after     = sg_xstrdup(news->items[i]);
			c->old_table = p->old_table;
			c->new_table = p->new_table;
			locate_shape(e, c, p);
		}
	}
}

static void locate_touched(emitter *e, const history *h, sg_change *c, const char *type, const char *name) {
	const mark *m = h ? find_mark(&h->touched, type, name) : NULL;

	if (e->located && m) {
		locate(c, true, m->at);
	}
}

/* Whether the table or one of its columns ends under another name than it started with: SQLite
 * rewrites the statements that name them, so their text no longer compares. A rebuilt table that
 * takes its old name back is not renamed. */
static bool renamed_parts(const pair *p) {
	if (sg_strcasecmp(p->old_table->name, p->new_table->name) != 0) {
		return true;
	}

	for (size_t i = 0; i < p->old_table->columns.count; ++i) {
		int to = p->map.items[i];

		if (to >= 0 && sg_strcasecmp(p->old_table->columns.items[i].name, p->new_table->columns.items[to].name) != 0) {
			return true;
		}
	}

	return false;
}

static void compare_pair(emitter *e, const history *h, const sg_model *old_model, const sg_model *new_model,
		pair *p) {
	const sg_table *o    = p->old_table;
	const sg_table *n    = p->new_table;
	char           *fo   = describe_flags(o);
	char           *fn   = describe_flags(n);
	bool            text = renamed_parts(p);

	if (strcmp(fo, fn) != 0) {
		sg_change *c = add(e, SG_CHANGE_ALTER, SG_THING_TABLE, n->name, NULL);

		c->before    = fo;
		c->after     = fn;
		c->old_table = o;
		c->new_table = n;
		fo = fn = NULL;
		locate_shape(e, c, p);
	}

	free(fo);
	free(fn);

	if (sg_strcasecmp(o->name, n->name) != 0) {
		sg_change *c = add(e, SG_CHANGE_RENAME, SG_THING_TABLE, n->name, NULL);

		c->old_name  = sg_xstrdup(o->name);
		c->old_table = o;
		c->new_table = n;

		if (e->located && p->line && p->line->renamed) {
			locate(c, true, p->line->renamed_at);
		}
	}

	/* Views and virtual tables: their statement. */
	if (n->kind != SG_TABLE && !text) {
		const sg_object *a = sg_model_object(old_model, o->kind == SG_VIEW ? "view" : "table", o->name);
		const sg_object *b = sg_model_object(new_model, n->kind == SG_VIEW ? "view" : "table", n->name);
		char            *x = normalized(a ? a->sql : "");
		char            *y = normalized(b ? b->sql : "");

		if (strcmp(x, y) != 0) {
			sg_change *c = add(e, SG_CHANGE_ALTER, SG_THING_TABLE, n->name, NULL);

			c->before    = x;
			c->after     = y;
			c->old_table = o;
			c->new_table = n;
			x = y = NULL;
			locate_shape(e, c, p);
		}

		free(x);
		free(y);
	}

	if (n->kind == SG_VIEW) {
		return;
	}

	/* Columns. */
	for (size_t i = 0; i < o->columns.count; ++i) {
		const sg_column *oc = &o->columns.items[i];

		if (p->map.items[i] < 0) {
			sg_change *c = add(e, SG_CHANGE_REMOVE, SG_THING_COLUMN, n->name, oc->name);

			c->before     = describe_column(oc);
			c->old_table  = o;
			c->new_table  = n;
			c->old_column = oc;
			locate_shape(e, c, p);
			continue;
		}

		const sg_column *nc = &n->columns.items[p->map.items[i]];
		char            *x  = describe_column(oc);
		char            *y  = describe_column(nc);

		if (sg_strcasecmp(oc->name, nc->name) != 0) {
			sg_change *c = add(e, SG_CHANGE_RENAME, SG_THING_COLUMN, n->name, nc->name);

			c->old_name   = sg_xstrdup(oc->name);
			c->old_table  = o;
			c->new_table  = n;
			c->old_column = oc;
			c->new_column = nc;

			for (size_t l = 0; p->line && l < p->line->columns.count; ++l) {
				const column_line *col = &p->line->columns.items[l];

				if (e->located && col->renamed && sg_strcasecmp(col->current, nc->name) == 0) {
					locate(c, true, col->renamed_at);
				}
			}
		}

		if (strcmp(x, y) != 0) {
			sg_change *c = add(e, SG_CHANGE_ALTER, SG_THING_COLUMN, n->name, nc->name);

			c->before     = x;
			c->after      = y;
			c->old_table  = o;
			c->new_table  = n;
			c->old_column = oc;
			c->new_column = nc;
			x = y = NULL;
			locate_shape(e, c, p);
		}

		free(x);
		free(y);
	}

	bool *matched = sg_xmalloc(n->columns.count + 1);

	memset(matched, 0, n->columns.count + 1);

	for (size_t i = 0; i < o->columns.count; ++i) {
		if (p->map.items[i] >= 0) {
			matched[p->map.items[i]] = true;
		}
	}

	for (size_t i = 0; i < n->columns.count; ++i) {
		if (!matched[i]) {
			sg_change *c = add(e, SG_CHANGE_ADD, SG_THING_COLUMN, n->name, n->columns.items[i].name);

			c->after      = describe_column(&n->columns.items[i]);
			c->old_table  = o;
			c->new_table  = n;
			c->new_column = &n->columns.items[i];
			locate_shape(e, c, p);
		}
	}

	/* The order of the columns both have. */
	sg_buf before = {0};
	sg_buf after  = {0};

	for (size_t i = 0; i < o->columns.count; ++i) {
		if (p->map.items[i] >= 0) {
			sg_buf_printf(&before, "%s%s", before.len ? ", " : "", n->columns.items[p->map.items[i]].name);
		}
	}

	for (size_t i = 0; i < n->columns.count; ++i) {
		if (matched[i]) {
			sg_buf_printf(&after, "%s%s", after.len ? ", " : "", n->columns.items[i].name);
		}
	}

	free(matched);

	if (before.len && strcmp(before.data, after.data) != 0) {
		sg_change *c = add(e, SG_CHANGE_ALTER, SG_THING_ORDER, n->name, NULL);

		c->before    = before.data;
		c->after     = after.data;
		c->old_table = o;
		c->new_table = n;
		before.data  = after.data = NULL;
		locate_shape(e, c, p);
	}

	sg_buf_free(&before);
	sg_buf_free(&after);

	/* Unique keys a constraint declares; a unique index is compared as an index. */
	texts olds = {0};
	texts news = {0};

	for (size_t i = 0; i < o->unique_keys.count; ++i) {
		if (is_constraint(o, &o->unique_keys.items[i])) {
			*SG_PUSH(olds) = describe_key(e->pairs, o, &o->unique_keys.items[i], true);
		}
	}

	for (size_t i = 0; i < n->unique_keys.count; ++i) {
		if (is_constraint(n, &n->unique_keys.items[i])) {
			*SG_PUSH(news) = describe_key(e->pairs, n, &n->unique_keys.items[i], false);
		}
	}

	compare_sets(e, p, SG_THING_UNIQUE, &olds, &news);
	free_texts(&olds);
	free_texts(&news);

	for (size_t i = 0; i < o->foreign_keys.count; ++i) {
		*SG_PUSH(olds) = describe_fk(e->pairs, o, &o->foreign_keys.items[i], true);
	}

	for (size_t i = 0; i < n->foreign_keys.count; ++i) {
		*SG_PUSH(news) = describe_fk(e->pairs, n, &n->foreign_keys.items[i], false);
	}

	compare_sets(e, p, SG_THING_FOREIGN_KEY, &olds, &news);
	free_texts(&olds);
	free_texts(&news);

	/* Indexes of their own, by name. */
	for (size_t i = 0; i < o->indexes.count; ++i) {
		const sg_index *oi = &o->indexes.items[i];
		const sg_index *ni = NULL;

		if (strcmp(oi->origin, "create") != 0) {
			continue;
		}

		for (size_t j = 0; j < n->indexes.count; ++j) {
			if (sg_strcasecmp(n->indexes.items[j].name, oi->name) == 0) {
				ni = &n->indexes.items[j];
			}
		}

		if (!ni) {
			sg_change *c = add(e, SG_CHANGE_REMOVE, SG_THING_INDEX, n->name, oi->name);

			c->before    = describe_index(e->pairs, old_model, o, oi, true, false);
			c->old_table = o;
			c->new_table = n;
			locate_touched(e, h, c, "index", oi->name);
			continue;
		}

		bool  sql = !text && (oi->partial || oi->expression || ni->partial || ni->expression);
		char *x   = describe_index(e->pairs, old_model, o, oi, true, sql);
		char *y   = describe_index(e->pairs, new_model, n, ni, false, sql);

		if (strcmp(x, y) != 0) {
			sg_change *c = add(e, SG_CHANGE_ALTER, SG_THING_INDEX, n->name, ni->name);

			c->before    = x;
			c->after     = y;
			c->old_table = o;
			c->new_table = n;
			x = y = NULL;
			locate_touched(e, h, c, "index", ni->name);
		}

		free(x);
		free(y);
	}

	for (size_t i = 0; i < n->indexes.count; ++i) {
		const sg_index *ni    = &n->indexes.items[i];
		bool            found = false;

		for (size_t j = 0; j < o->indexes.count; ++j) {
			found = found || sg_strcasecmp(o->indexes.items[j].name, ni->name) == 0;
		}

		if (!found && strcmp(ni->origin, "create") == 0) {
			sg_change *c = add(e, SG_CHANGE_ADD, SG_THING_INDEX, n->name, ni->name);

			c->after     = describe_index(e->pairs, new_model, n, ni, false, false);
			c->old_table = o;
			c->new_table = n;
			locate_touched(e, h, c, "index", ni->name);
		}
	}

	/* Triggers, by name and statement. */
	for (size_t i = 0; i < o->triggers.count; ++i) {
		const char      *name = o->triggers.items[i];
		const sg_object *a    = sg_model_object(old_model, "trigger", name);
		const sg_object *b    = sg_model_object(new_model, "trigger", name);

		if (!b || sg_strcasecmp(b->table, n->name) != 0) {
			sg_change *c = add(e, SG_CHANGE_REMOVE, SG_THING_TRIGGER, n->name, name);

			c->before    = normalized(a ? a->sql : "");
			c->old_table = o;
			c->new_table = n;
			locate_touched(e, h, c, "trigger", name);
		} else if (!text) {
			char *x = normalized(a ? a->sql : "");
			char *y = normalized(b->sql);

			if (strcmp(x, y) != 0) {
				sg_change *c = add(e, SG_CHANGE_ALTER, SG_THING_TRIGGER, n->name, name);

				c->before    = x;
				c->after     = y;
				c->old_table = o;
				c->new_table = n;
				x = y = NULL;
				locate_touched(e, h, c, "trigger", name);
			}

			free(x);
			free(y);
		}
	}

	for (size_t i = 0; i < n->triggers.count; ++i) {
		const char *name  = n->triggers.items[i];
		bool        found = false;

		for (size_t j = 0; j < o->triggers.count; ++j) {
			found = found || sg_strcasecmp(o->triggers.items[j], name) == 0;
		}

		if (!found) {
			const sg_object *b = sg_model_object(new_model, "trigger", name);
			sg_change       *c = add(e, SG_CHANGE_ADD, SG_THING_TRIGGER, n->name, name);

			c->after     = normalized(b ? b->sql : "");
			c->old_table = o;
			c->new_table = n;
			locate_touched(e, h, c, "trigger", name);
		}
	}
}

/* Which old column each new one continues: through the renames the steps saw, else by name. */
static void map_columns(pair *p) {
	for (size_t i = 0; i < p->old_table->columns.count; ++i) {
		const char *name = p->old_table->columns.items[i].name;
		int         to   = -1;

		for (size_t l = 0; p->line && l < p->line->columns.count; ++l) {
			if (sg_strcasecmp(p->line->columns.items[l].original, name) == 0) {
				name = p->line->columns.items[l].current;
			}
		}

		for (size_t j = 0; j < p->new_table->columns.count; ++j) {
			if (sg_strcasecmp(p->new_table->columns.items[j].name, name) == 0) {
				to = (int)j;
			}
		}

		*SG_PUSH(p->map) = to;
	}
}

static int compare_changes(const void *left, const void *right) {
	const sg_change *a = left;
	const sg_change *b = right;
	int              c = sg_strcasecmp(a->table, b->table);

	if (c == 0) {
		c = (int)a->thing - (int)b->thing;
	}

	if (c == 0) {
		c = sg_strcasecmp(a->name ? a->name : "", b->name ? b->name : "");
	}

	/* What goes before what replaces it. */
	static const int ORDER[] = { [SG_CHANGE_REMOVE] = 0, [SG_CHANGE_ADD] = 1, [SG_CHANGE_ALTER] = 2,
		[SG_CHANGE_RENAME] = 3 };

	if (c == 0) {
		c = ORDER[a->kind] - ORDER[b->kind];
	}

	if (c == 0) {
		c = strcmp(a->before ? a->before : a->after ? a->after : "", b->before ? b->before : b->after ? b->after : "");
	}

	return c;
}

void sg_diff(const sg_model *const *steps, const sg_origin *at, size_t count, sg_changes *out) {
	history         h       = {0};
	pairs           ps      = {0};
	const sg_model *first   = steps[0];
	const sg_model *last    = steps[count - 1];
	bool            located = at != NULL;

	memset(out, 0, sizeof *out);

	for (size_t i = 0; i < first->tables.count; ++i) {
		const sg_table *t    = &first->tables.items[i];
		lineage        *line = SG_PUSH(h.lines);

		line->original = t->name;
		line->current  = sg_xstrdup(t->name);
		line->alive    = true;

		for (size_t c = 0; c < t->columns.count; ++c) {
			column_line *col = SG_PUSH(line->columns);

			col->original = t->columns.items[c].name;
			col->current  = sg_xstrdup(t->columns.items[c].name);
		}
	}

	if (located) {
		for (size_t i = 1; i < count; ++i) {
			step(&h, steps[i - 1], steps[i], at[i], true);
		}
	}

	/* Pair the ends: through the lineage, else by name (without steps, or a rebuilt table). */
	bool *taken = sg_xmalloc(last->tables.count + 1);

	memset(taken, 0, last->tables.count + 1);

	for (size_t i = 0; i < first->tables.count; ++i) {
		const sg_table *o    = &first->tables.items[i];
		const sg_table *n    = NULL;
		lineage        *line = NULL;

		for (size_t l = 0; l < h.lines.count && located; ++l) {
			if (h.lines.items[l].alive && h.lines.items[l].original
					&& sg_strcasecmp(h.lines.items[l].original, o->name) == 0) {
				line = &h.lines.items[l];
				n    = sg_model_table(last, line->current);
			}
		}

		if (!n) {
			n    = sg_model_table(last, o->name);
			line = n && located ? line_named(&h, n->name) : NULL;

			/* A table that is still another table's past name belongs to that one. */
			if (line && line->original && sg_strcasecmp(line->original, o->name) != 0) {
				n = NULL;
			}
		}

		if (n && n->kind != o->kind && (n->kind == SG_VIEW || o->kind == SG_VIEW)) {
			n = NULL;
		}

		if (n) {
			pair *p = SG_PUSH(ps);

			p->old_table = o;
			p->new_table = n;
			p->line      = line;
			taken[n - last->tables.items] = true;
		}
	}

	for (size_t i = 0; i < ps.count; ++i) {
		map_columns(&ps.items[i]);
	}

	emitter e = { out, &ps, located };

	for (size_t i = 0; i < first->tables.count; ++i) {
		const sg_table *o      = &first->tables.items[i];
		bool            paired = false;

		for (size_t j = 0; j < ps.count; ++j) {
			paired = paired || ps.items[j].old_table == o;
		}

		if (!paired) {
			sg_change  *c = add(&e, SG_CHANGE_REMOVE, SG_THING_TABLE, o->name, NULL);
			const mark *m = find_mark(&h.dropped, "table", o->name);

			c->before    = describe_flags(o);
			c->old_table = o;

			if (located && m) {
				locate(c, true, m->at);
			}
		}
	}

	for (size_t i = 0; i < last->tables.count; ++i) {
		const sg_table *n = &last->tables.items[i];

		if (!taken[i]) {
			sg_change *c    = add(&e, SG_CHANGE_ADD, SG_THING_TABLE, n->name, NULL);
			lineage   *line = located ? line_named(&h, n->name) : NULL;

			c->after     = describe_flags(n);
			c->new_table = n;

			if (line && line->created) {
				locate(c, true, line->created_at);
			}
		}
	}

	free(taken);

	for (size_t i = 0; i < ps.count; ++i) {
		compare_pair(&e, located ? &h : NULL, first, last, &ps.items[i]);
	}

	if (out->count > 1) {
		qsort(out->items, out->count, sizeof *out->items, compare_changes);
	}

	for (size_t i = 0; i < ps.count; ++i) {
		free(ps.items[i].map.items);
	}

	free(ps.items);

	for (size_t i = 0; i < h.lines.count; ++i) {
		free(h.lines.items[i].current);

		for (size_t c = 0; c < h.lines.items[i].columns.count; ++c) {
			free(h.lines.items[i].columns.items[c].current);
		}

		free(h.lines.items[i].columns.items);
	}

	free(h.lines.items);

	for (size_t i = 0; i < h.dropped.count; ++i) {
		free(h.dropped.items[i].name);
	}

	for (size_t i = 0; i < h.touched.count; ++i) {
		free(h.touched.items[i].name);
	}

	free(h.dropped.items);
	free(h.touched.items);
}

/* ---- output -------------------------------------------------------------------------------- */

static const char *thing_word(const sg_change *c) {
	switch (c->thing) {
	case SG_THING_TABLE:       return kind_word(c->new_table ? c->new_table : c->old_table);
	case SG_THING_COLUMN:      return "column";
	case SG_THING_ORDER:       return "column order";
	case SG_THING_UNIQUE:      return "key";
	case SG_THING_FOREIGN_KEY: return "foreign key";
	case SG_THING_INDEX:       return "index";
	case SG_THING_TRIGGER:     return "trigger";
	}

	return "table";
}

static const char *kind_name(sg_change_kind kind) {
	static const char *const NAMES[] = { "add", "remove", "alter", "rename" };

	return NAMES[kind];
}

static void put_subject(sg_buf *out, const char *table, const char *name) {
	sg_buf_puts(out, table);

	if (name) {
		sg_buf_printf(out, ".%s", name);
	}
}

void sg_diff_write_text(const sg_changes *changes, sg_buf *out) {
	static const char SIGNS[] = { '+', '-', '~', '>' };

	for (size_t i = 0; i < changes->count; ++i) {
		const sg_change *c = &changes->items[i];

		sg_buf_printf(out, "%c %s ", SIGNS[c->kind], thing_word(c));

		if (c->kind == SG_CHANGE_RENAME) {
			bool column = c->thing == SG_THING_COLUMN;

			put_subject(out, column ? c->table : c->old_name, column ? c->old_name : NULL);
			sg_buf_puts(out, " → ");
			put_subject(out, c->table, c->name);
		} else {
			put_subject(out, c->table, c->name);

			if (c->kind == SG_CHANGE_ALTER) {
				sg_buf_printf(out, ": %s → %s", c->before, c->after);
			} else if (c->thing != SG_THING_TABLE && (c->before || c->after)) {
				sg_buf_printf(out, ": %s", c->kind == SG_CHANGE_ADD ? c->after : c->before);
			}
		}

		sg_buf_putc(out, '\n');
	}
}

void sg_diff_write_json(const sg_changes *changes, sg_buf *out) {
	sg_jw w = { out, 0, { true }, { false } };

	sg_jw_open(&w, NULL, '[', false);

	for (size_t i = 0; i < changes->count; ++i) {
		const sg_change *c = &changes->items[i];

		sg_jw_open(&w, NULL, '{', true);
		sg_jw_text(&w, "change", kind_name(c->kind));
		sg_jw_text(&w, "thing", thing_word(c));
		sg_jw_text(&w, "table", c->table);
		sg_jw_text(&w, "name", c->name);
		sg_jw_text(&w, "old_name", c->old_name);
		sg_jw_text(&w, "before", c->before);
		sg_jw_text(&w, "after", c->after);

		if (c->located) {
			sg_jw_open(&w, "at", '{', true);
			sg_jw_text(&w, "script", c->at.script);
			sg_jw_int(&w, "line", c->at.line);
			sg_jw_int(&w, "col", c->at.col);
			sg_jw_close(&w, '}');
		} else {
			sg_jw_null(&w, "at");
		}

		sg_jw_close(&w, '}');
	}

	sg_jw_close(&w, ']');
	sg_buf_putc(out, '\n');
}

void sg_changes_free(sg_changes *changes) {
	for (size_t i = 0; i < changes->count; ++i) {
		free(changes->items[i].table);
		free(changes->items[i].name);
		free(changes->items[i].old_name);
		free(changes->items[i].before);
		free(changes->items[i].after);
	}

	free(changes->items);
	memset(changes, 0, sizeof *changes);
}
