#include "model.h"

#include <stdio.h>

#include "jsonw.h"
#include "schema.h"

static void origin(sg_jw *w, const char *key, const sg_origin *created) {
	if (!created->script) {
		sg_jw_null(w, key);
		return;
	}

	sg_jw_open(w, key, '{', true);
	sg_jw_text(w, "script", created->script);
	sg_jw_int(w, "line", created->line);
	sg_jw_text(w, "patch", created->patch);
	sg_jw_bool(w, "system", created->system);
	sg_jw_close(w, '}');
}

static const char *kind_name(sg_table_kind kind) {
	switch (kind) {
	case SG_TABLE:   return "table";
	case SG_VIEW:    return "view";
	case SG_VIRTUAL: return "virtual";
	}

	return "table";
}

static void column(sg_jw *w, const sg_table *table, const sg_column *c) {
	sg_jw_open(w, NULL, '{', false);
	sg_jw_text(w, "name", c->name);
	sg_jw_text(w, "declared_type", c->declared_type);
	sg_jw_text(w, "affinity", sg_affinity_name(c->affinity));
	sg_jw_bool(w, "notnull", c->notnull);
	sg_jw_bool(w, "notnull_declared", c->notnull_declared);
	sg_jw_text(w, "default", c->default_sql);
	sg_jw_int(w, "primary_key", c->pk);
	sg_jw_text(w, "generated", c->generated == SG_GEN_VIRTUAL ? "virtual"
			: (c->generated == SG_GEN_STORED ? "stored" : NULL));
	sg_jw_bool(w, "hidden", c->hidden);
	sg_jw_text(w, "collation", c->collation);
	sg_jw_bool(w, "autoincrement", c->autoincrement);
	sg_jw_bool(w, "rowid_alias", c->rowid_alias);
	sg_jw_bool(w, "needs_quote", c->needs_quote);

	if (table->kind == SG_VIEW) {
		sg_jw_open(w, "origin", '{', true);
		sg_jw_text(w, "table", c->origin_table);
		sg_jw_text(w, "column", c->origin_column);
		sg_jw_close(w, '}');
	}

	sg_jw_close(w, '}');
}

static void index_(sg_jw *w, const sg_index *x) {
	sg_jw_open(w, NULL, '{', false);
	sg_jw_text(w, "name", x->name);
	sg_jw_bool(w, "unique", x->unique);
	sg_jw_text(w, "origin", x->origin);
	sg_jw_bool(w, "partial", x->partial);
	sg_jw_bool(w, "expression", x->expression);
	sg_jw_open(w, "columns", '[', false);

	for (size_t i = 0; i < x->columns.count; ++i) {
		sg_jw_open(w, NULL, '{', true);
		sg_jw_text(w, "name", x->columns.items[i][0] ? x->columns.items[i] : NULL);
		sg_jw_bool(w, "desc", x->descending.items[i]);
		sg_jw_text(w, "collation", x->collations.items[i]);
		sg_jw_close(w, '}');
	}

	sg_jw_close(w, ']');
	origin(w, "created", &x->created);
	sg_jw_close(w, '}');
}

static void table(sg_jw *w, const sg_table *t) {
	sg_jw_open(w, NULL, '{', false);
	sg_jw_text(w, "name", t->name);
	sg_jw_text(w, "kind", kind_name(t->kind));
	origin(w, "created", &t->created);
	sg_jw_bool(w, "without_rowid", t->without_rowid);
	sg_jw_bool(w, "strict", t->strict);
	sg_jw_bool(w, "autoincrement", t->autoincrement);
	sg_jw_text(w, "rowid_alias", t->rowid_alias >= 0 ? t->columns.items[t->rowid_alias].name : NULL);
	sg_jw_bool(w, "junction", t->junction);

	sg_jw_open(w, "columns", '[', false);

	for (size_t i = 0; i < t->columns.count; ++i) {
		column(w, t, &t->columns.items[i]);
	}

	sg_jw_close(w, ']');
	sg_jw_open(w, "indexes", '[', false);

	for (size_t i = 0; i < t->indexes.count; ++i) {
		index_(w, &t->indexes.items[i]);
	}

	sg_jw_close(w, ']');
	sg_jw_open(w, "unique_keys", '[', false);

	for (size_t i = 0; i < t->unique_keys.count; ++i) {
		const sg_unique_key *key = &t->unique_keys.items[i];

		sg_jw_open(w, NULL, '{', true);
		sg_jw_names(w, "columns", &key->columns);
		sg_jw_text(w, "source", key->source);
		sg_jw_bool(w, "total", key->total);
		sg_jw_close(w, '}');
	}

	sg_jw_close(w, ']');
	sg_jw_open(w, "foreign_keys", '[', false);

	for (size_t i = 0; i < t->foreign_keys.count; ++i) {
		const sg_foreign_key *fk = &t->foreign_keys.items[i];

		sg_jw_open(w, NULL, '{', false);
		sg_jw_names(w, "columns", &fk->columns);
		sg_jw_text(w, "parent", fk->parent);
		sg_jw_bool(w, "parent_exists", fk->parent_exists);
		sg_jw_names(w, "parent_columns", &fk->parent_columns);
		sg_jw_names(w, "resolved_parent_columns", &fk->resolved);
		sg_jw_text(w, "on_update", fk->on_update);
		sg_jw_text(w, "on_delete", fk->on_delete);
		sg_jw_bool(w, "indexed", fk->indexed);
		sg_jw_bool(w, "optional", fk->optional);
		sg_jw_bool(w, "unique", fk->unique);
		sg_jw_close(w, '}');
	}

	sg_jw_close(w, ']');
	sg_jw_names(w, "triggers", &t->triggers);
	sg_jw_close(w, '}');
}

void sg_model_write_json(const sg_model *model, const sg_patchset *patches, const char *database,
		sg_buf *out) {
	sg_jw w = { out, 0, { true }, { false } };

	sg_jw_open(&w, NULL, '{', false);
	sg_jw_int(&w, "format", 1);
	sg_jw_text(&w, "sqlite", model->sqlite_version);
	sg_jw_open(&w, "source", '{', false);

	if (patches) {
		sg_jw_text(&w, "systables", patches->systables);
		sg_jw_open(&w, "patches", '[', false);

		for (size_t i = 0; i < patches->patches.count; ++i) {
			const sg_patch *patch = &patches->patches.items[i];

			sg_jw_open(&w, NULL, '{', false);
			sg_jw_text(&w, "version", patch->version.text);
			sg_jw_text(&w, "directory", patch->dir);
			sg_jw_open(&w, "scripts", '[', true);

			for (size_t j = 0; j < patch->scripts.count; ++j) {
				sg_jw_text(&w, NULL, patch->scripts.items[j].name);
			}

			sg_jw_close(&w, ']');
			sg_jw_close(&w, '}');
		}

		sg_jw_close(&w, ']');
	} else {
		sg_jw_text(&w, "database", database);
	}

	sg_jw_close(&w, '}');
	sg_jw_open(&w, "tables", '[', false);

	for (size_t i = 0; i < model->tables.count; ++i) {
		table(&w, &model->tables.items[i]);
	}

	sg_jw_close(&w, ']');
	sg_jw_close(&w, '}');
	sg_buf_putc(out, '\n');
}
