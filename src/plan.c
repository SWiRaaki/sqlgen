#include "plan.h"

#include <fnmatch.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "jsonw.h"

static void emit_word(const char *word, void *user) {
	*SG_PUSH(*(sg_words *)user) = sg_xstrdup(word);
}

static void split_words(const char *name, sg_words *out) {
	sg_split_words(name, emit_word, out);
}

static void push_word(sg_words *words, const char *word) {
	*SG_PUSH(*words) = sg_xstrdup(word);
}

static void push_words(sg_words *into, const sg_words *from) {
	for (size_t i = 0; i < from->count; ++i) {
		push_word(into, from->items[i]);
	}
}

static void free_words(sg_words *words) {
	for (size_t i = 0; i < words->count; ++i) {
		free(words->items[i]);
	}

	free(words->items);
	memset(words, 0, sizeof *words);
}

static int column_index(const sg_plan_table *t, const char *name) {
	for (size_t i = 0; i < t->columns.count; ++i) {
		if (sg_strcasecmp(t->columns.items[i].column->name, name) == 0) {
			return (int)i;
		}
	}

	return -1;
}

typedef sg_index_list index_list;

static bool same_columns(const index_list *a, const index_list *b) {
	if (a->count != b->count) {
		return false;
	}

	for (size_t i = 0; i < a->count; ++i) {
		bool found = false;

		for (size_t j = 0; j < b->count && !found; ++j) {
			found = a->items[i] == b->items[j];
		}

		if (!found) {
			return false;
		}
	}

	return true;
}

/* Column indexes for a list of names; false when a name is no column (an expression part). */
static bool resolve(const sg_plan_table *t, const sg_names *names, size_t count, index_list *out) {
	for (size_t i = 0; i < count; ++i) {
		int at = column_index(t, names->items[i]);

		if (at < 0) {
			free(out->items);
			memset(out, 0, sizeof *out);
			return false;
		}

		*SG_PUSH(*out) = (size_t)at;
	}

	return true;
}

/* "by a and b", appended to @p words. */
static void by_words(const sg_plan_table *t, const index_list *columns, const char *by, sg_words *words) {
	push_word(words, by);

	for (size_t i = 0; i < columns->count; ++i) {
		if (i) {
			push_word(words, "and");
		}

		push_words(words, &t->columns.items[columns->items[i]].words);
	}
}

static sg_statement *add_statement(sg_plan_table *t, sg_stmt_kind kind, const char *first) {
	sg_statement *s = SG_PUSH(t->statements);

	s->kind = kind;
	push_word(&s->words, first);
	return s;
}

static void add_param(sg_statement *s, size_t column, bool optional) {
	sg_param *p = SG_PUSH(s->params);

	p->column   = column;
	p->optional = optional;
}

/* " where a = ?n and b = ?n+1", with the key's parameters appended to @p s. */
static void where(sg_plan_table *t, sg_statement *s, const index_list *key, sg_buf *sql) {
	for (size_t i = 0; i < key->count; ++i) {
		add_param(s, key->items[i], false);
		*SG_PUSH(s->key) = key->items[i];
		sg_buf_printf(sql, "%s %s = ?%zu", i ? " and" : " where",
				t->columns.items[key->items[i]].sql_name, s->params.count);
	}
}

/* The columns a list is ordered by: the rowid alias, else the primary key, else rowid itself. */
static void order_by(const sg_plan_table *t, sg_buf *sql) {
	const sg_table *table = t->table;

	if (table->rowid_alias >= 0) {
		sg_buf_printf(sql, " order by %s", t->columns.items[table->rowid_alias].sql_name);
		return;
	}

	bool first = true;

	for (int position = 1; position <= (int)t->columns.count; ++position) {
		for (size_t i = 0; i < t->columns.count; ++i) {
			if (t->columns.items[i].column->pk == position) {
				sg_buf_printf(sql, "%s%s", first ? " order by " : ", ", t->columns.items[i].sql_name);
				first = false;
			}
		}
	}

	if (first) {
		sg_buf_puts(sql, " order by rowid");
	}
}

static bool is_primary(const sg_table *table, const sg_unique_key *key) {
	if (strcmp(key->source, "rowid") == 0) {
		return true;
	}

	for (size_t i = 0; i < table->indexes.count; ++i) {
		if (strcmp(table->indexes.items[i].name, key->source) == 0) {
			return strcmp(table->indexes.items[i].origin, "primary_key") == 0;
		}
	}

	return false;
}

typedef SG_VEC(index_list) key_list;

static void free_keys(key_list *keys) {
	for (size_t i = 0; i < keys->count; ++i) {
		free(keys->items[i].items);
	}

	free(keys->items);
}

static bool listed(const key_list *keys, const index_list *key) {
	for (size_t i = 0; i < keys->count; ++i) {
		if (same_columns(&keys->items[i], key)) {
			return true;
		}
	}

	return false;
}

static void copy_list(index_list *into, const index_list *from) {
	for (size_t i = 0; i < from->count; ++i) {
		*SG_PUSH(*into) = from->items[i];
	}
}

/* A get per total unique key; the keys are collected into @p gets for the deletes. */
static void plan_gets(sg_plan_table *t, key_list *gets, index_list *primary) {
	const sg_table *table = t->table;

	for (size_t k = 0; k < table->unique_keys.count; ++k) {
		const sg_unique_key *key     = &table->unique_keys.items[k];
		index_list           columns = {0};

		if (!key->total || !resolve(t, &key->columns, key->columns.count, &columns)) {
			continue;
		}

		sg_statement *s   = add_statement(t, SG_STMT_GET, "get");
		sg_buf        sql = {0};

		if (is_primary(table, key)) {
			push_word(&s->words, "by");
			push_word(&s->words, "key");

			if (primary->count == 0) {
				copy_list(primary, &columns);
			}
		} else {
			by_words(t, &columns, "by", &s->words);
		}

		sg_buf_printf(&sql, "select %s from %s", t->select_list, t->sql_name);
		where(t, s, &columns, &sql);
		s->sql = sql.data;

		copy_list(SG_PUSH(*gets), &columns);
		free(columns.items);
	}
}

/* A list per leading prefix of each usable index, unless a get already covers those columns. */
static void plan_lists(sg_plan_table *t, const key_list *gets, key_list *lists) {
	const sg_table *table = t->table;

	for (size_t x = 0; x < table->indexes.count; ++x) {
		const sg_index *index = &table->indexes.items[x];

		if (index->partial || index->expression) {
			continue;
		}

		for (size_t n = 1; n <= index->columns.count; ++n) {
			index_list prefix = {0};

			if (!resolve(t, &index->columns, n, &prefix) || listed(gets, &prefix) || listed(lists, &prefix)) {
				free(prefix.items);
				continue;
			}

			sg_statement *s   = add_statement(t, SG_STMT_LIST, "list");
			sg_buf        sql = {0};

			by_words(t, &prefix, "by", &s->words);
			sg_buf_printf(&sql, "select %s from %s", t->select_list, t->sql_name);
			where(t, s, &prefix, &sql);
			order_by(t, &sql);
			s->sql = sql.data;

			copy_list(SG_PUSH(*lists), &prefix);
			free(prefix.items);
		}
	}
}

/* The columns an insert binds: every writable one, except an autoincrement rowid alias. */
static void insert_columns(const sg_plan_table *t, index_list *out) {
	for (size_t i = 0; i < t->columns.count; ++i) {
		const sg_plan_column *c = &t->columns.items[i];

		if (c->writable && !(c->column->rowid_alias && c->column->autoincrement)) {
			*SG_PUSH(*out) = i;
		}
	}
}

/* "insert into t (a, b) values (?1, coalesce(?2, (default)))", its parameters added to @p s. A
 * NOT NULL column with a default may be left unbound and takes the default; the rowid alias may be
 * left unbound and takes a new rowid. */
static void insert_values(sg_plan_table *t, sg_statement *s, const index_list *columns, const char *verb,
		sg_buf *sql) {
	sg_buf_printf(sql, "%s %s (", verb, t->sql_name);

	for (size_t i = 0; i < columns->count; ++i) {
		sg_buf_printf(sql, "%s%s", i ? ", " : "", t->columns.items[columns->items[i]].sql_name);
	}

	sg_buf_puts(sql, ") values (");

	for (size_t i = 0; i < columns->count; ++i) {
		const sg_plan_column *c        = &t->columns.items[columns->items[i]];
		bool                  defaults = c->column->notnull && c->column->default_sql;
		bool                  optional = defaults || c->column->rowid_alias;

		add_param(s, columns->items[i], optional);

		if (defaults) {
			sg_buf_printf(sql, "%scoalesce(?%zu, (%s))", i ? ", " : "", s->params.count, c->column->default_sql);
		} else {
			sg_buf_printf(sql, "%s?%zu", i ? ", " : "", s->params.count);
		}
	}

	sg_buf_putc(sql, ')');
}

static bool contains(const index_list *list, size_t value) {
	for (size_t i = 0; i < list->count; ++i) {
		if (list->items[i] == value) {
			return true;
		}
	}

	return false;
}

static void plan_insert(sg_plan_table *t, const index_list *columns, const index_list *primary) {
	sg_statement *s   = add_statement(t, SG_STMT_INSERT, "insert");
	sg_buf        sql = {0};

	insert_values(t, s, columns, "insert into", &sql);

	/* A rowid table's new key is sqlite3_last_insert_rowid; a WITHOUT ROWID table's is returned. */
	if (t->table->without_rowid) {
		for (size_t i = 0; i < primary->count; ++i) {
			sg_buf_printf(&sql, "%s%s", i ? ", " : " returning ", t->columns.items[primary->items[i]].sql_name);
			*SG_PUSH(s->returning) = primary->items[i];
		}
	}

	s->sql = sql.data;
}

static void plan_update(sg_plan_table *t, const index_list *primary) {
	index_list set = {0};

	for (size_t i = 0; i < t->columns.count; ++i) {
		const sg_plan_column *c = &t->columns.items[i];

		if (c->writable && !c->column->rowid_alias && !contains(primary, i)) {
			*SG_PUSH(set) = i;
		}
	}

	if (primary->count == 0 || set.count == 0) {
		free(set.items);
		return;
	}

	sg_statement *s   = add_statement(t, SG_STMT_UPDATE, "update");
	sg_buf        sql = {0};

	push_word(&s->words, "by");
	push_word(&s->words, "key");
	sg_buf_printf(&sql, "update %s set ", t->sql_name);

	for (size_t i = 0; i < set.count; ++i) {
		add_param(s, set.items[i], false);
		sg_buf_printf(&sql, "%s%s = ?%zu", i ? ", " : "", t->columns.items[set.items[i]].sql_name, s->params.count);
	}

	where(t, s, primary, &sql);
	s->sql = sql.data;
	free(set.items);
}

static void plan_deletes(sg_plan_table *t, const key_list *keys, const index_list *primary) {
	for (size_t k = 0; k < keys->count; ++k) {
		sg_statement *s   = add_statement(t, SG_STMT_DELETE, "delete");
		sg_buf        sql = {0};

		if (same_columns(&keys->items[k], primary)) {
			push_word(&s->words, "by");
			push_word(&s->words, "key");
		} else {
			by_words(t, &keys->items[k], "by", &s->words);
		}

		sg_buf_printf(&sql, "delete from %s", t->sql_name);
		where(t, s, &keys->items[k], &sql);
		s->sql = sql.data;
	}
}

/* An upsert per get key the insert binds in full; it updates every other inserted column. */
static void plan_upserts(sg_plan_table *t, const key_list *gets, const index_list *columns,
		const index_list *primary) {
	for (size_t k = 0; k < gets->count; ++k) {
		const index_list *key   = &gets->items[k];
		bool              bound = true;

		for (size_t i = 0; i < key->count; ++i) {
			bound = bound && contains(columns, key->items[i]);
		}

		if (!bound) {
			continue;
		}

		sg_statement *s       = add_statement(t, SG_STMT_UPSERT, "upsert");
		sg_buf        sql     = {0};
		bool          updates = false;

		if (same_columns(key, primary)) {
			push_word(&s->words, "on");
			push_word(&s->words, "key");
		} else {
			by_words(t, key, "on", &s->words);
		}

		insert_values(t, s, columns, "insert into", &sql);
		sg_buf_puts(&sql, " on conflict (");

		for (size_t i = 0; i < key->count; ++i) {
			sg_buf_printf(&sql, "%s%s", i ? ", " : "", t->columns.items[key->items[i]].sql_name);
			*SG_PUSH(s->key) = key->items[i];
		}

		sg_buf_puts(&sql, ") do ");

		for (size_t i = 0; i < columns->count; ++i) {
			size_t c = columns->items[i];

			if (contains(key, c) || t->columns.items[c].column->rowid_alias) {
				continue;
			}

			sg_buf_printf(&sql, "%s%s = excluded.%s", updates ? ", " : "update set ",
					t->columns.items[c].sql_name, t->columns.items[c].sql_name);
			updates = true;
		}

		if (!updates) {
			sg_buf_puts(&sql, "nothing");
		}

		s->sql = sql.data;
	}
}

static void plan_links(sg_plan_table *t) {
	sg_statement *link   = add_statement(t, SG_STMT_LINK, "link");
	sg_statement *unlink = add_statement(t, SG_STMT_UNLINK, "unlink");
	sg_buf        a      = {0};
	sg_buf        b      = {0};
	index_list    both   = {0};

	*SG_PUSH(both) = 0;
	*SG_PUSH(both) = 1;

	sg_buf_printf(&a, "insert or ignore into %s (%s, %s) values (?1, ?2)", t->sql_name,
			t->columns.items[0].sql_name, t->columns.items[1].sql_name);
	add_param(link, 0, false);
	add_param(link, 1, false);
	*SG_PUSH(link->key) = 0;
	*SG_PUSH(link->key) = 1;
	link->sql = a.data;

	sg_buf_printf(&b, "delete from %s", t->sql_name);
	where(t, unlink, &both, &b);
	unlink->sql = b.data;
	free(both.items);
}

/* The entry naming @p table itself, not "*"; NULL when there is none. */
static const sg_table_config *named_entry(const sg_config *config, const char *table) {
	for (size_t i = 0; i < config->tables.count; ++i) {
		if (sg_strcasecmp(config->tables.items[i].name, table) == 0) {
			return &config->tables.items[i];
		}
	}

	return NULL;
}

static bool plan_writes(sg_plan_table *t, const sg_config *config, const key_list *gets,
		const key_list *lists, const index_list *primary) {
	const sg_table        *table   = t->table;
	sg_table_options       options = sg_config_table(config, table->name);
	const sg_table_config *named   = named_entry(config, table->name);
	unsigned               writes  = options.writes;
	bool                   ok      = true;

	/* "*" asks for every table; a view or virtual table, or a link on a non-junction, is skipped
	 * there. An entry naming the table asks for it in particular, so it is refused instead. */
	if (table->kind != SG_TABLE) {
		if (named && named->options.has_writes && named->options.writes) {
			sg_error(config->path, 0, 0, "$.tables.%s.writes: %s is a %s, which cannot be written",
					named->name, table->name, table->kind == SG_VIEW ? "view" : "virtual table");
			ok = false;
		}

		return ok;
	}

	if ((writes & SG_WRITE_LINK) && !table->junction) {
		if (named && named->options.has_writes && (named->options.writes & SG_WRITE_LINK)) {
			sg_error(config->path, 0, 0, "$.tables.%s.writes: link needs a junction -- two integer"
					" columns, each a foreign key, unique together -- and %s is none", named->name, table->name);
			ok = false;
		}

		writes &= ~(unsigned)SG_WRITE_LINK;
	}

	index_list columns = {0};

	insert_columns(t, &columns);

	if (writes & SG_WRITE_INSERT) {
		plan_insert(t, &columns, primary);
	}

	if (writes & SG_WRITE_UPDATE) {
		plan_update(t, primary);
	}

	if (writes & SG_WRITE_DELETE) {
		plan_deletes(t, gets, primary);
		plan_deletes(t, lists, primary);
	}

	if (writes & SG_WRITE_UPSERT) {
		plan_upserts(t, gets, &columns, primary);
	}

	if (writes & SG_WRITE_LINK) {
		plan_links(t);
	}

	free(columns.items);
	return ok;
}

static char *joined(const sg_words *words) {
	sg_buf out = {0};

	for (size_t i = 0; i < words->count; ++i) {
		sg_buf_puts(&out, i ? " " : "");
		sg_buf_puts(&out, words->items[i]);
	}

	return out.data ? out.data : sg_xstrdup("");
}

/* Names come from words, so two statements whose words agree would get one name in any language. */
static bool distinct_names(const sg_plan_table *t, const sg_config *config) {
	bool ok = true;

	for (size_t i = 0; i < t->statements.count; ++i) {
		char *a = joined(&t->statements.items[i].words);

		for (size_t j = i + 1; j < t->statements.count; ++j) {
			char *b = joined(&t->statements.items[j].words);

			if (strcmp(a, b) == 0) {
				sg_error(config->path, 0, 0, "two statements of %s would both be named \"%s\"",
						t->table->name, a);
				ok = false;
			}

			free(b);
		}

		free(a);
	}

	return ok;
}

const char *sg_plan_logical_type(const sg_config *config, const sg_column *column, bool *custom) {
	const char *type = sg_affinity_name(column->affinity);

	*custom = false;

	for (size_t i = 0; i < config->types_by_name.count; ++i) {
		if (fnmatch(config->types_by_name.items[i].key, column->name, 0) == 0) {
			type    = config->types_by_name.items[i].value;
			*custom = true;
		}
	}

	for (size_t i = 0; i < config->types_by_declared.count; ++i) {
		if (sg_strcasecmp(config->types_by_declared.items[i].key, column->declared_type) == 0) {
			type    = config->types_by_declared.items[i].value;
			*custom = true;
		}
	}

	return type;
}

const char *sg_plan_id_of(const sg_model *model, const sg_table *table, const sg_column *column) {
	if (column->rowid_alias) {
		return table->name;
	}

	for (size_t i = 0; i < table->foreign_keys.count; ++i) {
		const sg_foreign_key *fk     = &table->foreign_keys.items[i];
		const sg_table       *parent = sg_model_table(model, fk->parent);

		if (fk->columns.count != 1 || fk->resolved.count != 1 || !parent || parent->rowid_alias < 0
				|| sg_strcasecmp(fk->columns.items[0], column->name) != 0) {
			continue;
		}

		if (sg_strcasecmp(fk->resolved.items[0], parent->columns.items[parent->rowid_alias].name) == 0) {
			return parent->name;
		}
	}

	return NULL;
}

static bool plan_table(sg_plan *plan, const sg_model *model, const sg_table *table) {
	const sg_config *config = plan->config;
	sg_plan_table   *t      = SG_PUSH(plan->tables);
	sg_buf           select = {0};

	t->table     = table;
	t->sql_name  = sg_sql_identifier(table->name);
	t->id_column = -1;
	split_words(table->name, &t->words);

	for (size_t i = 0; i < table->columns.count; ++i) {
		const sg_column *column = &table->columns.items[i];
		sg_plan_column  *c      = SG_PUSH(t->columns);

		c->column   = column;
		c->sql_name = sg_sql_identifier(column->name);
		c->nullable = !column->notnull;
		c->writable = table->kind == SG_TABLE && column->generated == SG_GEN_NONE && !column->hidden;
		c->id_of    = config->typed_ids ? sg_plan_id_of(model, table, column) : NULL;
		c->type     = sg_xstrdup(sg_plan_logical_type(config, column, &c->custom));
		split_words(column->name, &c->words);

		if (column->rowid_alias && c->id_of) {
			t->id_column = (int)i;
		}

		if (!column->hidden) {
			sg_buf_printf(&select, "%s%s", select.len ? ", " : "", c->sql_name);
		}
	}

	t->select_list = select.data ? select.data : sg_xstrdup("");

	if (table->kind != SG_TABLE) {
		return plan_writes(t, config, NULL, NULL, NULL);
	}

	key_list   gets    = {0};
	key_list   lists   = {0};
	index_list primary = {0};

	plan_gets(t, &gets, &primary);
	plan_lists(t, &gets, &lists);

	bool ok = plan_writes(t, config, &gets, &lists, &primary) && distinct_names(t, config);

	free_keys(&gets);
	free_keys(&lists);
	free(primary.items);
	return ok;
}

static bool read_script(sg_plan_script *out, const char *name, const char *path) {
	size_t len = 0;

	out->name = sg_xstrdup(name);
	out->path = sg_xstrdup(path);
	return sg_read_file(path, &out->text, &len);
}

static void hash_bytes(uint64_t *hash, const char *bytes, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		*hash ^= (unsigned char)bytes[i];
		*hash *= 0x100000001b3ULL;
	}
}

static void hash_script(uint64_t *hash, const sg_plan_script *script) {
	hash_bytes(hash, script->name, strlen(script->name) + 1);
	hash_bytes(hash, script->text, strlen(script->text) + 1);
}

static bool plan_scripts(sg_plan *plan, const sg_patchset *patches) {
	uint64_t hash = 0xcbf29ce484222325ULL;
	bool     ok   = true;

	if (patches->systables) {
		const char *slash = strrchr(patches->systables, '/');

		ok = read_script(&plan->systables, slash ? slash + 1 : patches->systables, patches->systables);

		if (ok) {
			hash_script(&hash, &plan->systables);
		}
	}

	for (size_t i = 0; ok && i < patches->patches.count; ++i) {
		const sg_patch *patch = &patches->patches.items[i];
		sg_plan_patch  *p     = SG_PUSH(plan->patches);

		p->patch = patch;

		for (size_t j = 0; ok && j < patch->scripts.count; ++j) {
			sg_plan_script *script = SG_PUSH(p->scripts);

			ok = read_script(script, patch->scripts.items[j].name, patch->scripts.items[j].path);

			if (ok) {
				hash_script(&hash, script);
			}
		}
	}

	for (int i = 15; i >= 0; --i) {
		plan->hash[i] = "0123456789abcdef"[hash & 0xF];
		hash >>= 4;
	}

	plan->hash[16] = '\0';
	return ok;
}

static void add_custom_type(sg_plan *plan, const char *type) {
	for (size_t i = 0; i < plan->custom_types.count; ++i) {
		if (strcmp(plan->custom_types.items[i], type) == 0) {
			return;
		}
	}

	size_t at = plan->custom_types.count;

	*SG_PUSH(plan->custom_types) = sg_xstrdup(type);

	while (at > 0 && strcmp(plan->custom_types.items[at - 1], plan->custom_types.items[at]) > 0) {
		char *held = plan->custom_types.items[at - 1];

		plan->custom_types.items[at - 1] = plan->custom_types.items[at];
		plan->custom_types.items[at]     = held;
		--at;
	}
}

static void collect_custom_types(sg_plan *plan) {
	for (size_t t = 0; t < plan->tables.count; ++t) {
		for (size_t c = 0; c < plan->tables.items[t].columns.count; ++c) {
			if (plan->tables.items[t].columns.items[c].custom) {
				add_custom_type(plan, plan->tables.items[t].columns.items[c].type);
			}
		}
	}

	for (size_t q = 0; plan->queries && q < plan->queries->count; ++q) {
		const sg_query *query = &plan->queries->items[q];

		for (size_t i = 0; i < query->params.count; ++i) {
			if (query->params.items[i].custom) {
				add_custom_type(plan, query->params.items[i].type);
			}
		}

		for (size_t i = 0; i < query->columns.count; ++i) {
			if (query->columns.items[i].custom) {
				add_custom_type(plan, query->columns.items[i].type);
			}
		}
	}
}

bool sg_plan_build(const sg_model *model, const sg_patchset *patches, const sg_config *config,
		const sg_queries *queries, sg_plan *out) {
	bool ok = true;

	memset(out, 0, sizeof *out);
	out->config  = config;
	out->queries = queries;
	out->model   = model;
	out->patchset = patches;

	for (size_t i = 0; i < model->tables.count; ++i) {
		ok = plan_table(out, model, &model->tables.items[i]) && ok;
	}

	collect_custom_types(out);
	return plan_scripts(out, patches) && ok;
}

const char *sg_stmt_kind_name(sg_stmt_kind kind) {
	switch (kind) {
	case SG_STMT_GET:    return "get";
	case SG_STMT_LIST:   return "list";
	case SG_STMT_INSERT: return "insert";
	case SG_STMT_UPDATE: return "update";
	case SG_STMT_DELETE: return "delete";
	case SG_STMT_UPSERT: return "upsert";
	case SG_STMT_LINK:   return "link";
	case SG_STMT_UNLINK: return "unlink";
	}

	return "get";
}

static void column_names(sg_jw *w, const char *key, const sg_plan_table *t, const index_list *columns) {
	sg_jw_open(w, key, '[', true);

	for (size_t i = 0; i < columns->count; ++i) {
		sg_jw_text(w, NULL, t->columns.items[columns->items[i]].column->name);
	}

	sg_jw_close(w, ']');
}

static void write_script(sg_jw *w, const char *key, const sg_plan_script *script) {
	sg_jw_open(w, key, '{', false);
	sg_jw_text(w, "name", script->name);
	sg_jw_text(w, "path", script->path);
	sg_jw_text(w, "text", script->text);
	sg_jw_close(w, '}');
}

static const char *kind_name(sg_table_kind kind) {
	return kind == SG_VIEW ? "view" : (kind == SG_VIRTUAL ? "virtual" : "table");
}

/* The words naming a query's row type: the table's whose row it reuses, or its own. */
static const sg_words *row_words(const sg_plan *plan, const sg_query *q) {
	for (size_t t = 0; q->row_table && t < plan->tables.count; ++t) {
		if (plan->tables.items[t].table == q->row_table) {
			return &plan->tables.items[t].words;
		}
	}

	return &q->words;
}

/* A query's parameters or columns, with the fields a table's columns have where they apply. */
static void write_fields(sg_jw *w, const char *key, const sg_query_fields *fields) {
	sg_jw_open(w, key, '[', false);

	for (size_t i = 0; i < fields->count; ++i) {
		const sg_query_field *f = &fields->items[i];

		sg_jw_open(w, NULL, '{', true);
		sg_jw_text(w, "name", f->name);
		sg_jw_names(w, "words", &f->words);

		if (f->index) {
			sg_jw_int(w, "index", f->index);
		}

		sg_jw_text(w, "type", f->type);
		sg_jw_bool(w, "custom", f->custom);
		sg_jw_text(w, "affinity", sg_affinity_name(f->affinity));
		sg_jw_bool(w, "nullable", f->nullable);
		sg_jw_text(w, "id_of", f->id_of);
		sg_jw_close(w, '}');
	}

	sg_jw_close(w, ']');
}

void sg_plan_write_json(const sg_plan *plan, sg_buf *out) {
	sg_jw w = { out, 0, { true }, { false } };

	sg_jw_open(&w, NULL, '{', false);
	sg_jw_int(&w, "format", 1);
	sg_jw_text(&w, "hash", plan->hash);
	sg_jw_bool(&w, "embed_scripts", plan->config->embed_scripts);
	sg_jw_bool(&w, "typed_ids", plan->config->typed_ids);
	sg_jw_names(&w, "custom_types", &plan->custom_types);

	sg_jw_open(&w, "tables", '[', false);

	for (size_t i = 0; i < plan->tables.count; ++i) {
		const sg_plan_table *t = &plan->tables.items[i];

		sg_jw_open(&w, NULL, '{', false);
		sg_jw_text(&w, "name", t->table->name);
		sg_jw_names(&w, "words", &t->words);
		sg_jw_text(&w, "kind", kind_name(t->table->kind));
		sg_jw_bool(&w, "without_rowid", t->table->without_rowid);
		sg_jw_bool(&w, "junction", t->table->junction);
		sg_jw_text(&w, "sql_name", t->sql_name);

		if (t->table->created.script) {
			sg_jw_open(&w, "origin", '{', true);
			sg_jw_text(&w, "script", t->table->created.script);
			sg_jw_int(&w, "line", t->table->created.line);
			sg_jw_text(&w, "patch", t->table->created.patch);
			sg_jw_close(&w, '}');
		} else {
			sg_jw_null(&w, "origin");
		}
		sg_jw_text(&w, "id_column", t->id_column >= 0 ? t->columns.items[t->id_column].column->name : NULL);
		sg_jw_text(&w, "select_list", t->select_list);

		sg_jw_open(&w, "columns", '[', false);

		for (size_t c = 0; c < t->columns.count; ++c) {
			const sg_plan_column *col = &t->columns.items[c];

			sg_jw_open(&w, NULL, '{', true);
			sg_jw_text(&w, "name", col->column->name);
			sg_jw_names(&w, "words", &col->words);
			sg_jw_text(&w, "sql_name", col->sql_name);
			sg_jw_text(&w, "type", col->type);
			sg_jw_bool(&w, "custom", col->custom);
			sg_jw_text(&w, "affinity", sg_affinity_name(col->column->affinity));
			sg_jw_bool(&w, "nullable", col->nullable);
			sg_jw_bool(&w, "writable", col->writable);
			sg_jw_bool(&w, "rowid", col->column->rowid_alias);
			sg_jw_text(&w, "id_of", col->id_of);
			sg_jw_close(&w, '}');
		}

		sg_jw_close(&w, ']');
		sg_jw_open(&w, "statements", '[', false);

		for (size_t s = 0; s < t->statements.count; ++s) {
			const sg_statement *st = &t->statements.items[s];

			sg_jw_open(&w, NULL, '{', false);
			sg_jw_text(&w, "kind", sg_stmt_kind_name(st->kind));
			sg_jw_names(&w, "words", &st->words);
			sg_jw_text(&w, "sql", st->sql);
			sg_jw_open(&w, "params", '[', true);

			for (size_t p = 0; p < st->params.count; ++p) {
				sg_jw_open(&w, NULL, '{', true);
				sg_jw_text(&w, "column", t->columns.items[st->params.items[p].column].column->name);
				sg_jw_int(&w, "index", (long long)st->params.items[p].column);
				sg_jw_bool(&w, "optional", st->params.items[p].optional);
				sg_jw_close(&w, '}');
			}

			sg_jw_close(&w, ']');
			column_names(&w, "key", t, &st->key);
			column_names(&w, "returning", t, &st->returning);
			sg_jw_close(&w, '}');
		}

		sg_jw_close(&w, ']');
		sg_jw_close(&w, '}');
	}

	sg_jw_close(&w, ']');
	sg_jw_open(&w, "queries", '[', false);

	static const char *const rows[] = { "none", "one", "exactly_one", "many" };

	for (size_t i = 0; plan->queries && i < plan->queries->count; ++i) {
		const sg_query *q = &plan->queries->items[i];

		sg_jw_open(&w, NULL, '{', false);
		sg_jw_text(&w, "name", q->name);
		sg_jw_names(&w, "words", &q->words);
		sg_jw_text(&w, "path", q->path);
		sg_jw_names(&w, "doc", &q->doc);
		sg_jw_text(&w, "sql", q->sql);
		sg_jw_bool(&w, "writes", q->writes);
		sg_jw_text(&w, "rows", rows[q->rows]);
		sg_jw_text(&w, "row_table", q->row_table ? q->row_table->name : NULL);
		sg_jw_names(&w, "row_words", row_words(plan, q));
		write_fields(&w, "params", &q->params);
		write_fields(&w, "columns", &q->columns);
		sg_jw_close(&w, '}');
	}

	sg_jw_close(&w, ']');
	sg_jw_open(&w, "scripts", '{', false);

	if (plan->systables.name) {
		write_script(&w, "systables", &plan->systables);
	} else {
		sg_jw_null(&w, "systables");
	}

	sg_jw_open(&w, "patches", '[', false);

	for (size_t i = 0; i < plan->patches.count; ++i) {
		const sg_plan_patch *p = &plan->patches.items[i];

		sg_jw_open(&w, NULL, '{', false);
		sg_jw_text(&w, "version", p->patch->version.text);
		sg_jw_int(&w, "major", (long long)p->patch->version.major);
		sg_jw_int(&w, "minor", (long long)p->patch->version.minor);
		sg_jw_int(&w, "patch", (long long)p->patch->version.patch);
		sg_jw_text(&w, "prerelease", p->patch->version.prerelease);
		sg_jw_text(&w, "build", p->patch->version.build);
		sg_jw_open(&w, "scripts", '[', false);

		for (size_t j = 0; j < p->scripts.count; ++j) {
			write_script(&w, NULL, &p->scripts.items[j]);
		}

		sg_jw_close(&w, ']');
		sg_jw_close(&w, '}');
	}

	sg_jw_close(&w, ']');
	sg_jw_close(&w, '}');
	sg_jw_close(&w, '}');
	sg_buf_putc(out, '\n');
}

static void free_script(sg_plan_script *script) {
	free(script->name);
	free(script->path);
	free(script->text);
}

void sg_plan_free(sg_plan *plan) {
	for (size_t i = 0; i < plan->tables.count; ++i) {
		sg_plan_table *t = &plan->tables.items[i];

		free_words(&t->words);
		free(t->sql_name);
		free(t->select_list);

		for (size_t c = 0; c < t->columns.count; ++c) {
			free_words(&t->columns.items[c].words);
			free(t->columns.items[c].type);
			free(t->columns.items[c].sql_name);
		}

		free(t->columns.items);

		for (size_t s = 0; s < t->statements.count; ++s) {
			sg_statement *st = &t->statements.items[s];

			free_words(&st->words);
			free(st->sql);
			free(st->params.items);
			free(st->key.items);
			free(st->returning.items);
		}

		free(t->statements.items);
	}

	free(plan->tables.items);
	free_script(&plan->systables);

	for (size_t i = 0; i < plan->patches.count; ++i) {
		for (size_t j = 0; j < plan->patches.items[i].scripts.count; ++j) {
			free_script(&plan->patches.items[i].scripts.items[j]);
		}

		free(plan->patches.items[i].scripts.items);
	}

	free(plan->patches.items);
	for (size_t i = 0; i < plan->custom_types.count; ++i) {
		free(plan->custom_types.items[i]);
	}

	free(plan->custom_types.items);
	memset(plan, 0, sizeof *plan);
}
