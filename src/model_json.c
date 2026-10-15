#include "model.h"

#include <stdio.h>

#include "schema.h"

enum { MAX_DEPTH = 16 };

/* A small pretty-printer. A container opened inline keeps all of its children on one line. */
typedef struct {
	sg_buf *out;
	int     depth;
	bool    first[MAX_DEPTH];
	bool    inline_[MAX_DEPTH];
} writer;

static void indent(writer *w) {
	sg_buf_putc(w->out, '\n');

	for (int i = 0; i < w->depth; ++i) {
		sg_buf_putc(w->out, '\t');
	}
}

static void string(writer *w, const char *text) {
	sg_buf_putc(w->out, '"');

	for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
		switch (*c) {
		case '"':  sg_buf_puts(w->out, "\\\""); break;
		case '\\': sg_buf_puts(w->out, "\\\\"); break;
		case '\n': sg_buf_puts(w->out, "\\n");  break;
		case '\r': sg_buf_puts(w->out, "\\r");  break;
		case '\t': sg_buf_puts(w->out, "\\t");  break;
		default:
			if (*c < 0x20) {
				sg_buf_printf(w->out, "\\u%04x", *c);
			} else {
				sg_buf_putc(w->out, (char)*c);
			}
		}
	}

	sg_buf_putc(w->out, '"');
}

/* Separator, line break and key for the next member of the current container. */
static void member(writer *w, const char *key) {
	if (w->depth > 0) {
		if (!w->first[w->depth]) {
			sg_buf_putc(w->out, ',');
		}

		if (w->inline_[w->depth]) {
			if (!w->first[w->depth]) {
				sg_buf_putc(w->out, ' ');
			}
		} else {
			indent(w);
		}

		w->first[w->depth] = false;
	}

	if (key) {
		string(w, key);
		sg_buf_puts(w->out, ": ");
	}
}

static void open_(writer *w, const char *key, char brace, bool inline_) {
	bool parent_inline = w->depth > 0 && w->inline_[w->depth];

	member(w, key);
	sg_buf_putc(w->out, brace);
	++w->depth;
	w->first[w->depth]   = true;
	w->inline_[w->depth] = inline_ || parent_inline;
}

static void close_(writer *w, char brace) {
	bool empty = w->first[w->depth];
	bool flat  = w->inline_[w->depth];

	--w->depth;

	if (!empty && !flat) {
		indent(w);
	}

	sg_buf_putc(w->out, brace);
}

static void text(writer *w, const char *key, const char *value) {
	member(w, key);

	if (value) {
		string(w, value);
	} else {
		sg_buf_puts(w->out, "null");
	}
}

static void boolean(writer *w, const char *key, bool value) {
	member(w, key);
	sg_buf_puts(w->out, value ? "true" : "false");
}

static void integer(writer *w, const char *key, long long value) {
	member(w, key);
	sg_buf_printf(w->out, "%lld", value);
}

static void null(writer *w, const char *key) {
	member(w, key);
	sg_buf_puts(w->out, "null");
}

static void names(writer *w, const char *key, const sg_names *list) {
	open_(w, key, '[', true);

	for (size_t i = 0; i < list->count; ++i) {
		text(w, NULL, list->items[i]);
	}

	close_(w, ']');
}

static void origin(writer *w, const char *key, const sg_origin *created) {
	if (!created->script) {
		null(w, key);
		return;
	}

	open_(w, key, '{', true);
	text(w, "script", created->script);
	text(w, "patch", created->patch);
	boolean(w, "system", created->system);
	close_(w, '}');
}

static const char *kind_name(sg_table_kind kind) {
	switch (kind) {
	case SG_TABLE:   return "table";
	case SG_VIEW:    return "view";
	case SG_VIRTUAL: return "virtual";
	}

	return "table";
}

static void column(writer *w, const sg_table *table, const sg_column *c) {
	open_(w, NULL, '{', false);
	text(w, "name", c->name);
	text(w, "declared_type", c->declared_type);
	text(w, "affinity", sg_affinity_name(c->affinity));
	boolean(w, "notnull", c->notnull);
	boolean(w, "notnull_declared", c->notnull_declared);
	text(w, "default", c->default_sql);
	integer(w, "primary_key", c->pk);
	text(w, "generated", c->generated == SG_GEN_VIRTUAL ? "virtual"
			: (c->generated == SG_GEN_STORED ? "stored" : NULL));
	boolean(w, "hidden", c->hidden);
	text(w, "collation", c->collation);
	boolean(w, "autoincrement", c->autoincrement);
	boolean(w, "rowid_alias", c->rowid_alias);
	boolean(w, "needs_quote", c->needs_quote);

	if (table->kind == SG_VIEW) {
		open_(w, "origin", '{', true);
		text(w, "table", c->origin_table);
		text(w, "column", c->origin_column);
		close_(w, '}');
	}

	close_(w, '}');
}

static void index_(writer *w, const sg_index *x) {
	open_(w, NULL, '{', false);
	text(w, "name", x->name);
	boolean(w, "unique", x->unique);
	text(w, "origin", x->origin);
	boolean(w, "partial", x->partial);
	boolean(w, "expression", x->expression);
	open_(w, "columns", '[', false);

	for (size_t i = 0; i < x->columns.count; ++i) {
		open_(w, NULL, '{', true);
		text(w, "name", x->columns.items[i][0] ? x->columns.items[i] : NULL);
		boolean(w, "desc", x->descending.items[i]);
		text(w, "collation", x->collations.items[i]);
		close_(w, '}');
	}

	close_(w, ']');
	origin(w, "created", &x->created);
	close_(w, '}');
}

static void table(writer *w, const sg_table *t) {
	open_(w, NULL, '{', false);
	text(w, "name", t->name);
	text(w, "kind", kind_name(t->kind));
	origin(w, "created", &t->created);
	boolean(w, "without_rowid", t->without_rowid);
	boolean(w, "strict", t->strict);
	boolean(w, "autoincrement", t->autoincrement);
	text(w, "rowid_alias", t->rowid_alias >= 0 ? t->columns.items[t->rowid_alias].name : NULL);
	boolean(w, "junction", t->junction);

	open_(w, "columns", '[', false);

	for (size_t i = 0; i < t->columns.count; ++i) {
		column(w, t, &t->columns.items[i]);
	}

	close_(w, ']');
	open_(w, "indexes", '[', false);

	for (size_t i = 0; i < t->indexes.count; ++i) {
		index_(w, &t->indexes.items[i]);
	}

	close_(w, ']');
	open_(w, "unique_keys", '[', false);

	for (size_t i = 0; i < t->unique_keys.count; ++i) {
		const sg_unique_key *key = &t->unique_keys.items[i];

		open_(w, NULL, '{', true);
		names(w, "columns", &key->columns);
		text(w, "source", key->source);
		boolean(w, "total", key->total);
		close_(w, '}');
	}

	close_(w, ']');
	open_(w, "foreign_keys", '[', false);

	for (size_t i = 0; i < t->foreign_keys.count; ++i) {
		const sg_foreign_key *fk = &t->foreign_keys.items[i];

		open_(w, NULL, '{', false);
		names(w, "columns", &fk->columns);
		text(w, "parent", fk->parent);
		boolean(w, "parent_exists", fk->parent_exists);
		names(w, "parent_columns", &fk->parent_columns);
		names(w, "resolved_parent_columns", &fk->resolved);
		text(w, "on_update", fk->on_update);
		text(w, "on_delete", fk->on_delete);
		boolean(w, "indexed", fk->indexed);
		close_(w, '}');
	}

	close_(w, ']');
	names(w, "triggers", &t->triggers);
	close_(w, '}');
}

void sg_model_write_json(const sg_model *model, const sg_patchset *patches, const char *database,
		sg_buf *out) {
	writer w = { out, 0, { true }, { false } };

	open_(&w, NULL, '{', false);
	integer(&w, "format", 1);
	text(&w, "sqlite", model->sqlite_version);
	open_(&w, "source", '{', false);

	if (patches) {
		text(&w, "systables", patches->systables);
		open_(&w, "patches", '[', false);

		for (size_t i = 0; i < patches->patches.count; ++i) {
			const sg_patch *patch = &patches->patches.items[i];

			open_(&w, NULL, '{', false);
			text(&w, "version", patch->version.text);
			text(&w, "directory", patch->dir);
			open_(&w, "scripts", '[', true);

			for (size_t j = 0; j < patch->scripts.count; ++j) {
				text(&w, NULL, patch->scripts.items[j].name);
			}

			close_(&w, ']');
			close_(&w, '}');
		}

		close_(&w, ']');
	} else {
		text(&w, "database", database);
	}

	close_(&w, '}');
	open_(&w, "tables", '[', false);

	for (size_t i = 0; i < model->tables.count; ++i) {
		table(&w, &model->tables.items[i]);
	}

	close_(&w, ']');
	close_(&w, '}');
	sg_buf_putc(out, '\n');
}
