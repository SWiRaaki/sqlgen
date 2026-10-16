#include "model.h"

#include <stdlib.h>
#include <string.h>

static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
	sqlite3_stmt *stmt = NULL;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		sg_error(NULL, 0, 0, "introspection failed: %s (in: %s)", sqlite3_errmsg(db), sql);
		return NULL;
	}

	return stmt;
}

static char *column_dup(sqlite3_stmt *stmt, int index) {
	return sg_xstrdup((const char *)sqlite3_column_text(stmt, index));
}

static bool finish(sqlite3 *db, sqlite3_stmt *stmt, int rc) {
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		sg_error(NULL, 0, 0, "introspection failed: %s", sqlite3_errmsg(db));
		return false;
	}

	return true;
}

static sg_affinity affinity_of(const char *declared, bool strict) {
	if (strict && sg_strcasecmp(declared, "ANY") == 0) {
		return SG_AFF_ANY;
	}

	if (sg_contains_nocase(declared, "INT")) {
		return SG_AFF_INTEGER;
	}

	if (sg_contains_nocase(declared, "CHAR") || sg_contains_nocase(declared, "CLOB")
			|| sg_contains_nocase(declared, "TEXT")) {
		return SG_AFF_TEXT;
	}

	if (declared[0] == '\0' || sg_contains_nocase(declared, "BLOB")) {
		return SG_AFF_BLOB;
	}

	if (sg_contains_nocase(declared, "REAL") || sg_contains_nocase(declared, "FLOA")
			|| sg_contains_nocase(declared, "DOUB")) {
		return SG_AFF_REAL;
	}

	return SG_AFF_NUMERIC;
}

static bool plain_identifier(const char *name) {
	if (!(name[0] == '_' || (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) {
		return false;
	}

	for (const char *c = name; *c; ++c) {
		if (!(*c == '_' || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
				|| (*c >= '0' && *c <= '9'))) {
			return false;
		}
	}

	return true;
}

bool sg_needs_quote(const char *name) {
	return !plain_identifier(name) || sqlite3_keyword_check(name, (int)strlen(name)) != 0;
}

static char *quote_identifier(const char *name) {
	sg_buf out = {0};

	sg_buf_putc(&out, '"');

	for (const char *c = name; *c; ++c) {
		if (*c == '"') {
			sg_buf_putc(&out, '"');
		}

		sg_buf_putc(&out, *c);
	}

	sg_buf_putc(&out, '"');
	return out.data;
}

static bool read_columns(sqlite3 *db, sg_table *table) {
	sqlite3_stmt *stmt = prepare(db, "select name, type, \"notnull\", dflt_value, pk, hidden"
			" from pragma_table_xinfo(?1) order by cid");
	int           rc;

	if (!stmt) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, table->name, -1, SQLITE_STATIC);

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		sg_column *column = SG_PUSH(table->columns);
		int        hidden = sqlite3_column_int(stmt, 5);

		column->name             = column_dup(stmt, 0);
		column->declared_type    = column_dup(stmt, 1);
		column->notnull_declared = sqlite3_column_int(stmt, 2) != 0;
		column->default_sql      = column_dup(stmt, 3);
		column->pk               = sqlite3_column_int(stmt, 4);
		column->generated        = hidden == 2 ? SG_GEN_VIRTUAL : (hidden == 3 ? SG_GEN_STORED : SG_GEN_NONE);
		column->hidden           = hidden == 1;
		column->needs_quote      = sg_needs_quote(column->name);

		if (!column->declared_type) {
			column->declared_type = sg_xstrdup("");
		}

		column->affinity = affinity_of(column->declared_type, table->strict);
	}

	if (!finish(db, stmt, rc)) {
		return false;
	}

	if (table->kind == SG_VIEW) {
		return true;
	}

	for (size_t i = 0; i < table->columns.count; ++i) {
		sg_column  *column    = &table->columns.items[i];
		const char *type      = NULL;
		const char *collation = NULL;
		int         notnull   = 0;
		int         pk        = 0;
		int         autoinc   = 0;

		if (sqlite3_table_column_metadata(db, "main", table->name, column->name, &type, &collation,
				&notnull, &pk, &autoinc) != SQLITE_OK) {
			/* A virtual table's module may not describe its columns this way. */
			if (table->kind == SG_TABLE) {
				sg_error(NULL, 0, 0, "sqlite3_table_column_metadata(%s.%s) failed: %s",
						table->name, column->name, sqlite3_errmsg(db));
				return false;
			}

			continue;
		}

		column->collation     = sg_xstrdup(collation);
		column->autoincrement = autoinc != 0;
		table->autoincrement  = table->autoincrement || column->autoincrement;
	}

	return true;
}

/* Where each view column's value comes from, as SQLite resolves it through every alias. */
static bool read_view_origins(sqlite3 *db, sg_table *view) {
	char         *quoted = quote_identifier(view->name);
	char         *sql    = sg_format("select * from %s", quoted);
	sqlite3_stmt *stmt   = prepare(db, sql);

	free(sql);
	free(quoted);

	if (!stmt) {
		return false;
	}

	int count = sqlite3_column_count(stmt);

	for (int i = 0; i < count && (size_t)i < view->columns.count; ++i) {
		view->columns.items[i].origin_table  = sg_xstrdup(sqlite3_column_table_name(stmt, i));
		view->columns.items[i].origin_column = sg_xstrdup(sqlite3_column_origin_name(stmt, i));
	}

	sqlite3_finalize(stmt);
	return true;
}

static const char *origin_name(const char *code) {
	if (strcmp(code, "c") == 0) {
		return "create";
	}

	if (strcmp(code, "u") == 0) {
		return "unique";
	}

	return strcmp(code, "pk") == 0 ? "primary_key" : code;
}

static bool read_indexes(sqlite3 *db, sg_table *table) {
	sqlite3_stmt *list = prepare(db, "select name, \"unique\", origin, partial"
			" from pragma_index_list(?1) order by name");
	sqlite3_stmt *parts = prepare(db, "select name, \"desc\", coll, cid"
			" from pragma_index_xinfo(?1) where key = 1 order by seqno");
	int           rc;

	if (!list || !parts) {
		sqlite3_finalize(list);
		sqlite3_finalize(parts);
		return false;
	}

	sqlite3_bind_text(list, 1, table->name, -1, SQLITE_STATIC);

	while ((rc = sqlite3_step(list)) == SQLITE_ROW) {
		sg_index *index = SG_PUSH(table->indexes);

		index->name    = column_dup(list, 0);
		index->unique  = sqlite3_column_int(list, 1) != 0;
		index->origin  = sg_xstrdup(origin_name((const char *)sqlite3_column_text(list, 2)));
		index->partial = sqlite3_column_int(list, 3) != 0;

		sqlite3_reset(parts);
		sqlite3_bind_text(parts, 1, index->name, -1, SQLITE_STATIC);

		int prc;

		while ((prc = sqlite3_step(parts)) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(parts, 0);

			/* cid -2 is an expression; -1 would be the rowid itself, which is a column too. */
			if (sqlite3_column_int(parts, 3) == -2) {
				index->expression = true;
			}

			*SG_PUSH(index->columns)    = sg_xstrdup(name ? name : (sqlite3_column_int(parts, 3) == -1 ? "rowid" : ""));
			*SG_PUSH(index->descending) = sqlite3_column_int(parts, 1) != 0;
			*SG_PUSH(index->collations) = column_dup(parts, 2);
		}

		if (prc != SQLITE_DONE) {
			sqlite3_finalize(parts);
			return finish(db, list, prc);
		}
	}

	sqlite3_finalize(parts);
	return finish(db, list, rc);
}

static bool read_foreign_keys(sqlite3 *db, sg_table *table) {
	sqlite3_stmt *stmt = prepare(db, "select id, \"table\", \"from\", \"to\", on_update, on_delete"
			" from pragma_foreign_key_list(?1) order by id, seq");
	int           rc;

	if (!stmt) {
		return false;
	}

	sqlite3_bind_text(stmt, 1, table->name, -1, SQLITE_STATIC);

	sg_foreign_key *current = NULL;

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		int id = sqlite3_column_int(stmt, 0);

		if (!current || current->id != id) {
			current            = SG_PUSH(table->foreign_keys);
			current->id        = id;
			current->parent    = column_dup(stmt, 1);
			current->on_update = column_dup(stmt, 4);
			current->on_delete = column_dup(stmt, 5);
		}

		*SG_PUSH(current->columns) = column_dup(stmt, 2);

		if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
			*SG_PUSH(current->parent_columns) = column_dup(stmt, 3);
		}
	}

	return finish(db, stmt, rc);
}

static bool read_triggers(sqlite3 *db, sg_model *model) {
	sqlite3_stmt *stmt = prepare(db, "select name, tbl_name from sqlite_schema"
			" where type = 'trigger' order by name");
	int           rc;

	if (!stmt) {
		return false;
	}

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		sg_table *table = sg_model_table(model, (const char *)sqlite3_column_text(stmt, 1));

		if (table) {
			*SG_PUSH(table->triggers) = column_dup(stmt, 0);
		}
	}

	return finish(db, stmt, rc);
}

/* Objects internal to SQLite or to the generated migration are left out, as they are from the tables. */
static bool read_objects(sqlite3 *db, sg_model *model) {
	sqlite3_stmt *stmt = prepare(db, "select type, name, tbl_name, sql from sqlite_schema"
			" where sql is not null and substr(name, 1, 7) <> 'sqlite_' collate nocase"
			" and substr(tbl_name, 1, 5) <> '_sys_' collate nocase order by type, name");
	int           rc;

	if (!stmt) {
		return false;
	}

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		sg_object *object = SG_PUSH(model->objects);

		object->type  = column_dup(stmt, 0);
		object->name  = column_dup(stmt, 1);
		object->table = column_dup(stmt, 2);
		object->sql   = column_dup(stmt, 3);
	}

	return finish(db, stmt, rc);
}

/* Whether the first @p count names of @p left are, in any order, exactly the names of @p right. */
static bool same_set(const sg_names *left, const sg_names *right, size_t count) {
	if (left->count < count || right->count != count) {
		return false;
	}

	for (size_t i = 0; i < count; ++i) {
		bool found = false;

		for (size_t j = 0; j < count && !found; ++j) {
			found = sg_strcasecmp(left->items[i], right->items[j]) == 0;
		}

		if (!found) {
			return false;
		}
	}

	return true;
}

static void push_names(sg_names *into, const sg_names *from) {
	for (size_t i = 0; i < from->count; ++i) {
		*SG_PUSH(*into) = sg_xstrdup(from->items[i]);
	}
}

static void derive_keys(sg_table *table) {
	size_t pk_columns = 0;
	bool   pk_index   = false;

	table->rowid_alias = -1;

	for (size_t i = 0; i < table->columns.count; ++i) {
		pk_columns += table->columns.items[i].pk > 0;
	}

	for (size_t i = 0; i < table->indexes.count; ++i) {
		pk_index = pk_index || strcmp(table->indexes.items[i].origin, "primary_key") == 0;
	}

	/* The rowid alias is the one primary key SQLite builds no index for: `integer primary key
	 * desc`, `int primary key` and every WITHOUT ROWID key all get an index with origin 'pk'. */
	if (table->kind == SG_TABLE && !table->without_rowid && pk_columns == 1 && !pk_index) {
		for (size_t i = 0; i < table->columns.count; ++i) {
			sg_column *column = &table->columns.items[i];

			if (column->pk == 1 && sg_strcasecmp(column->declared_type, "INTEGER") == 0) {
				column->rowid_alias = true;
				table->rowid_alias  = (int)i;
			}
		}
	}

	for (size_t i = 0; i < table->columns.count; ++i) {
		sg_column *column = &table->columns.items[i];

		column->notnull = column->notnull_declared || column->rowid_alias
				|| (table->without_rowid && column->pk > 0);
	}

	if (table->rowid_alias >= 0) {
		sg_unique_key *key = SG_PUSH(table->unique_keys);

		*SG_PUSH(key->columns) = sg_xstrdup(table->columns.items[table->rowid_alias].name);
		key->source = sg_xstrdup("rowid");
		key->total  = true;
	}

	for (size_t i = 0; i < table->indexes.count; ++i) {
		sg_index *index = &table->indexes.items[i];

		if (!index->unique || index->partial || index->expression) {
			continue;
		}

		bool duplicate = false;

		for (size_t k = 0; k < table->unique_keys.count && !duplicate; ++k) {
			duplicate = same_set(&table->unique_keys.items[k].columns, &index->columns,
					index->columns.count)
					&& table->unique_keys.items[k].columns.count == index->columns.count;
		}

		if (duplicate) {
			continue;
		}

		sg_unique_key *key   = SG_PUSH(table->unique_keys);
		bool           total = true;

		push_names(&key->columns, &index->columns);
		key->source = sg_xstrdup(index->name);

		for (size_t c = 0; c < index->columns.count; ++c) {
			sg_column *column = sg_table_column(table, index->columns.items[c]);

			total = total && column && column->notnull;
		}

		key->total = total;
	}
}

static void derive_foreign_keys(sg_model *model, sg_table *table) {
	for (size_t i = 0; i < table->foreign_keys.count; ++i) {
		sg_foreign_key *fk     = &table->foreign_keys.items[i];
		sg_table       *parent = sg_model_table(model, fk->parent);

		fk->parent_exists = parent != NULL;

		if (fk->parent_columns.count > 0) {
			push_names(&fk->resolved, &fk->parent_columns);
		} else if (parent) {
			/* Omitted parent columns mean the parent's primary key, in key order. */
			for (int position = 1; position <= (int)parent->columns.count; ++position) {
				for (size_t c = 0; c < parent->columns.count; ++c) {
					if (parent->columns.items[c].pk == position) {
						*SG_PUSH(fk->resolved) = sg_xstrdup(parent->columns.items[c].name);
					}
				}
			}
		}

		size_t n = fk->columns.count;

		for (size_t x = 0; x < table->indexes.count && !fk->indexed; ++x) {
			fk->indexed = same_set(&table->indexes.items[x].columns, &fk->columns, n);
		}

		if (!fk->indexed && n == 1 && table->rowid_alias >= 0) {
			fk->indexed = sg_strcasecmp(fk->columns.items[0],
					table->columns.items[table->rowid_alias].name) == 0;
		}
	}
}

static bool covered_by_foreign_key(const sg_table *table, const char *column) {
	for (size_t i = 0; i < table->foreign_keys.count; ++i) {
		const sg_foreign_key *fk = &table->foreign_keys.items[i];

		for (size_t c = 0; c < fk->columns.count; ++c) {
			if (sg_strcasecmp(fk->columns.items[c], column) == 0) {
				return true;
			}
		}
	}

	return false;
}

static void derive_junction(sg_table *table) {
	if (table->kind != SG_TABLE || table->columns.count != 2) {
		return;
	}

	sg_names pair = {0};
	bool     ok   = true;

	for (size_t i = 0; i < 2; ++i) {
		const sg_column *column = &table->columns.items[i];

		ok = ok && column->affinity == SG_AFF_INTEGER && column->generated == SG_GEN_NONE
				&& covered_by_foreign_key(table, column->name);
		*SG_PUSH(pair) = column->name;
	}

	for (size_t k = 0; ok && k < table->unique_keys.count && !table->junction; ++k) {
		const sg_unique_key *key = &table->unique_keys.items[k];

		table->junction = key->total && key->columns.count == 2 && same_set(&key->columns, &pair, 2);
	}

	free(pair.items);
}

static int compare_foreign_keys(const void *left, const void *right) {
	const sg_foreign_key *l = left;
	const sg_foreign_key *r = right;

	for (size_t i = 0; i < l->columns.count && i < r->columns.count; ++i) {
		int order = strcmp(l->columns.items[i], r->columns.items[i]);

		if (order != 0) {
			return order;
		}
	}

	if (l->columns.count != r->columns.count) {
		return l->columns.count < r->columns.count ? -1 : 1;
	}

	return l->id - r->id;
}

/* Whether a view's statement compiles: within a patch, a table it reads may be mid-rebuild. */
static bool view_readable(sqlite3 *db, const char *name) {
	char         *quoted = quote_identifier(name);
	char         *sql    = sg_format("select * from %s", quoted);
	sqlite3_stmt *stmt   = NULL;
	bool          ok     = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK;

	sqlite3_finalize(stmt);
	free(sql);
	free(quoted);
	return ok;
}

static bool introspect(sqlite3 *db, sg_model *out, bool lenient) {
	sqlite3_stmt *stmt = prepare(db, "select name, type, wr, strict from pragma_table_list"
			" where schema = 'main' and type in ('table', 'view', 'virtual')"
			" and substr(name, 1, 7) <> 'sqlite_' collate nocase"
			" and substr(name, 1, 5) <> '_sys_' collate nocase order by name");
	int           rc;

	memset(out, 0, sizeof *out);
	out->sqlite_version = sg_xstrdup(sqlite3_libversion());

	if (!stmt) {
		return false;
	}

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		sg_table   *table = SG_PUSH(out->tables);
		const char *type  = (const char *)sqlite3_column_text(stmt, 1);

		table->name          = column_dup(stmt, 0);
		table->kind          = strcmp(type, "view") == 0 ? SG_VIEW
				: (strcmp(type, "virtual") == 0 ? SG_VIRTUAL : SG_TABLE);
		table->without_rowid = sqlite3_column_int(stmt, 2) != 0;
		table->strict        = sqlite3_column_int(stmt, 3) != 0;
		table->rowid_alias   = -1;
	}

	if (!finish(db, stmt, rc)) {
		return false;
	}

	for (size_t i = 0; i < out->tables.count; ++i) {
		sg_table *table = &out->tables.items[i];

		if (lenient && table->kind == SG_VIEW && !view_readable(db, table->name)) {
			continue;
		}

		if (!read_columns(db, table)) {
			return false;
		}

		if (table->kind == SG_VIEW) {
			if (!read_view_origins(db, table)) {
				return false;
			}

			continue;
		}

		if (!read_indexes(db, table) || !read_foreign_keys(db, table)) {
			return false;
		}

		derive_keys(table);
	}

	for (size_t i = 0; i < out->tables.count; ++i) {
		sg_table *table = &out->tables.items[i];

		derive_foreign_keys(out, table);
		derive_junction(table);

		if (table->foreign_keys.count > 1) {
			qsort(table->foreign_keys.items, table->foreign_keys.count, sizeof *table->foreign_keys.items,
					compare_foreign_keys);
		}
	}

	return read_triggers(db, out) && read_objects(db, out);
}

bool sg_introspect(sqlite3 *db, sg_model *out) {
	return introspect(db, out, false);
}

bool sg_introspect_between(sqlite3 *db, sg_model *out) {
	return introspect(db, out, true);
}

sg_table *sg_model_table(const sg_model *model, const char *name) {
	for (size_t i = 0; i < model->tables.count; ++i) {
		if (sg_strcasecmp(model->tables.items[i].name, name) == 0) {
			return &model->tables.items[i];
		}
	}

	return NULL;
}

const sg_object *sg_model_object(const sg_model *model, const char *type, const char *name) {
	for (size_t i = 0; i < model->objects.count; ++i) {
		const sg_object *object = &model->objects.items[i];

		if (strcmp(object->type, type) == 0 && sg_strcasecmp(object->name, name) == 0) {
			return object;
		}
	}

	return NULL;
}

sg_column *sg_table_column(const sg_table *table, const char *name) {
	for (size_t i = 0; i < table->columns.count; ++i) {
		if (sg_strcasecmp(table->columns.items[i].name, name) == 0) {
			return &table->columns.items[i];
		}
	}

	return NULL;
}

const char *sg_affinity_name(sg_affinity affinity) {
	switch (affinity) {
	case SG_AFF_INTEGER: return "integer";
	case SG_AFF_TEXT:    return "text";
	case SG_AFF_BLOB:    return "blob";
	case SG_AFF_REAL:    return "real";
	case SG_AFF_NUMERIC: return "numeric";
	case SG_AFF_ANY:     return "any";
	}

	return "blob";
}

static void free_names(sg_names *names) {
	for (size_t i = 0; i < names->count; ++i) {
		free(names->items[i]);
	}

	free(names->items);
}

void sg_model_free(sg_model *model) {
	for (size_t t = 0; t < model->tables.count; ++t) {
		sg_table *table = &model->tables.items[t];

		free(table->name);

		for (size_t i = 0; i < table->columns.count; ++i) {
			sg_column *column = &table->columns.items[i];

			free(column->name);
			free(column->declared_type);
			free(column->default_sql);
			free(column->collation);
			free(column->origin_table);
			free(column->origin_column);
		}

		free(table->columns.items);

		for (size_t i = 0; i < table->indexes.count; ++i) {
			sg_index *index = &table->indexes.items[i];

			free(index->name);
			free(index->origin);
			free_names(&index->columns);
			free(index->descending.items);
			free_names(&index->collations);
		}

		free(table->indexes.items);

		for (size_t i = 0; i < table->unique_keys.count; ++i) {
			free_names(&table->unique_keys.items[i].columns);
			free(table->unique_keys.items[i].source);
		}

		free(table->unique_keys.items);

		for (size_t i = 0; i < table->foreign_keys.count; ++i) {
			sg_foreign_key *fk = &table->foreign_keys.items[i];

			free_names(&fk->columns);
			free(fk->parent);
			free_names(&fk->parent_columns);
			free_names(&fk->resolved);
			free(fk->on_update);
			free(fk->on_delete);
		}

		free(table->foreign_keys.items);
		free_names(&table->triggers);
	}

	free(model->tables.items);

	for (size_t i = 0; i < model->objects.count; ++i) {
		free(model->objects.items[i].type);
		free(model->objects.items[i].name);
		free(model->objects.items[i].table);
		free(model->objects.items[i].sql);
	}

	free(model->objects.items);
	free(model->sqlite_version);
	memset(model, 0, sizeof *model);
}

char *sg_sql_identifier(const char *name) {
	return sg_needs_quote(name) ? quote_identifier(name) : sg_xstrdup(name);
}
