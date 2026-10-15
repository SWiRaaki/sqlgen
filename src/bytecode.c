#include "bytecode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_PASSES = 64 };

/* A NULL from clearing an aggregate's registers. SQLite clears them together with a flag,
 * `Null` then `Integer 0 rF`, sets the flag once it has filled them, and only reads them after
 * `IfPos rF` found it set -- so on that edge, such NULLs are dropped. Every other NULL is data. */
#define CLEARED (1u << 6)
#define NULLISH (SG_BT_NULL | CLEARED)

/* ---- the program ------------------------------------------------------------------------- */

typedef struct {
	char       op[24];
	long long  p1;
	long long  p2;
	long long  p3;
	char      *p4;
	int        p5;
	char      *comment;
} insn;

typedef SG_VEC(insn) program;

/* ---- abstract values ---------------------------------------------------------------------- */

enum {
	K_VALUE,
	K_RECORD,          /* a record built by the MakeRecord at ref */
	K_CURSOR_RECORD,   /* a record read from the records cursor ref */
	K_ADDRESS          /* a return address */
};

typedef struct {
	unsigned  types;      /* SG_BT_*; 0 = not reached */
	int       kind;
	int       table;      /* model table index, -1 none */
	int       column;     /* -1 rowid or none */
	int       param;      /* 1-based, 0 none */
	long long ref;
	bool      has_constant;
	long long constant;
} aval;

static aval value(unsigned types) {
	aval v = { types, K_VALUE, -1, -1, 0, 0, false, 0 };
	return v;
}

static bool same(const aval *a, const aval *b) {
	return a->types == b->types && a->kind == b->kind && a->table == b->table && a->column == b->column
			&& a->param == b->param && a->ref == b->ref && a->has_constant == b->has_constant
			&& a->constant == b->constant;
}

static aval join(const aval *a, const aval *b) {
	if (!a->types) {
		return *b;
	}

	if (!b->types) {
		return *a;
	}

	/* A cleared register joined with a value is that value, possibly not yet set. */
	if (a->types == CLEARED && a->kind == K_VALUE && a->table < 0 && !a->param) {
		aval out = *b;

		out.types |= CLEARED;
		return out;
	}

	if (b->types == CLEARED && b->kind == K_VALUE && b->table < 0 && !b->param) {
		aval out = *a;

		out.types |= CLEARED;
		return out;
	}

	aval out = *a;

	out.types |= b->types;

	if (a->kind != b->kind || (a->kind != K_VALUE && a->ref != b->ref)) {
		out = value(a->types | b->types | SG_BT_UNKNOWN);
		return out;
	}

	if (a->table != b->table || a->column != b->column) {
		out.table  = -1;
		out.column = -1;
	}

	if (a->param != b->param) {
		out.param = 0;
	}

	if (!a->has_constant || !b->has_constant || a->constant != b->constant) {
		out.has_constant = false;
	}

	return out;
}

/* ---- cursors ------------------------------------------------------------------------------ */

enum {
	C_NONE,
	C_TABLE,
	C_INDEX,
	C_RECORDS,   /* sorter, ephemeral table, automatic index: holds what is inserted */
	C_PSEUDO     /* reads the record in a register */
};

typedef struct {
	long long        number;
	size_t           address;      /* where it is opened */
	int              kind;
	int              table;
	const sg_index  *index;
	long long        reg;          /* C_PSEUDO */
	bool             null_row;
	SG_VEC(long long) records;     /* C_RECORDS: the MakeRecord addresses inserted */
} cursor;

typedef struct {
	sg_bparam  info;
	bool       linked;     /* a source was recorded */
} param_info;

typedef struct {
	sqlite3         *db;
	const sg_model  *model;
	const char      *file;
	program          ops;
	size_t           nreg;
	SG_VEC(cursor)   cursors;
	aval           **records;      /* per address: the joined elements of its MakeRecord, or NULL */
	aval           **acc;          /* per register: an aggregate's first argument */
	SG_VEC(param_info) params;
	SG_VEC(aval)     results;
	bool             changed;      /* something global grew this pass */
	size_t           at;           /* the instruction being followed */
	bool            *flag;         /* per register: an aggregate's "filled" flag, which IfPos tests */
	bool            *clears;       /* per address: a Null clearing what such a flag guards */
} analysis;

static unsigned cleared(const analysis *a, size_t at) {
	return a->clears[at] ? CLEARED : SG_BT_NULL;
}

/* Flags: registers some IfPos tests and some Integer sets to 0. A Null right before such a reset
 * clears what the flag guards. */
static void find_flags(analysis *a) {
	for (size_t n = 0; n < a->ops.count; ++n) {
		const insn *i = &a->ops.items[n];

		if (strcmp(i->op, "IfPos") == 0 && i->p1 >= 0 && (size_t)i->p1 < a->nreg) {
			a->flag[i->p1] = true;
		}
	}

	for (size_t n = 0; n + 1 < a->ops.count; ++n) {
		const insn *i    = &a->ops.items[n];
		const insn *next = &a->ops.items[n + 1];

		a->clears[n] = strcmp(i->op, "Null") == 0 && strcmp(next->op, "Integer") == 0 && next->p1 == 0
				&& next->p2 >= 0 && (size_t)next->p2 < a->nreg && a->flag[next->p2];
	}
}

/* ---- reading the program ------------------------------------------------------------------ */

static bool read_program(analysis *a, const char *sql) {
	char         *text = sqlite3_mprintf("explain %s", sql);
	sqlite3_stmt *stmt = NULL;
	int           rc   = sqlite3_prepare_v2(a->db, text, -1, &stmt, NULL);

	sqlite3_free(text);

	if (rc != SQLITE_OK) {
		sg_error(a->file, 0, 0, "explain failed: %s", sqlite3_errmsg(a->db));
		return false;
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		insn       *i  = SG_PUSH(a->ops);
		const char *op = (const char *)sqlite3_column_text(stmt, 1);

		snprintf(i->op, sizeof i->op, "%s", op ? op : "");
		i->p1      = sqlite3_column_int64(stmt, 2);
		i->p2      = sqlite3_column_int64(stmt, 3);
		i->p3      = sqlite3_column_int64(stmt, 4);
		i->p4      = sg_xstrdup((const char *)sqlite3_column_text(stmt, 5));
		i->p5      = sqlite3_column_int(stmt, 6);
		i->comment = sg_xstrdup((const char *)sqlite3_column_text(stmt, 7));
	}

	sqlite3_finalize(stmt);

	/* Registers are numbered from 1; every operand is at most the highest one an instruction can
	 * name, plus the widest range it covers. */
	long long top = 1;

	for (size_t n = 0; n < a->ops.count; ++n) {
		const insn *i = &a->ops.items[n];
		long long   m = i->p1 + i->p2 + i->p3 + 2;

		top = m > top ? m : top;
	}

	a->nreg = (size_t)top;
	return true;
}

static bool is(const insn *i, const char *op) {
	return strcmp(i->op, op) == 0;
}

static long long p4_int(const insn *i) {
	return i->p4 ? atoll(i->p4) : 0;
}

/* ---- cursors ------------------------------------------------------------------------------ */

/* SQLite reuses cursor numbers: a use refers to the nearest opening of that number before it. */
static cursor *cursor_at(analysis *a, long long n) {
	cursor *best = NULL;

	for (size_t i = 0; i < a->cursors.count; ++i) {
		cursor *c = &a->cursors.items[i];

		if (c->number == n && (c->address <= a->at || !best) && (!best || c->address > best->address
				|| best->address > a->at)) {
			best = c;
		}
	}

	return best;
}

static cursor *opened(analysis *a, long long n) {
	cursor *c = SG_PUSH(a->cursors);

	c->number  = n;
	c->address = a->at;
	c->table   = -1;
	return c;
}

static int table_index(const analysis *a, const sg_table *t) {
	return t ? (int)(t - a->model->tables.items) : -1;
}

/* The object with root page @p root, from sqlite_schema. */
static void open_cursor(analysis *a, const insn *i) {
	cursor       *c    = opened(a, i->p1);
	sqlite3_stmt *stmt = NULL;

	if (i->p3 != 0) {
		return;
	}

	sqlite3_prepare_v2(a->db, "select type, name, tbl_name from sqlite_schema where rootpage = ?1", -1,
			&stmt, NULL);
	sqlite3_bind_int64(stmt, 1, i->p2);

	if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char     *type  = (const char *)sqlite3_column_text(stmt, 0);
		const char     *name  = (const char *)sqlite3_column_text(stmt, 1);
		const sg_table *table = sg_model_table(a->model, (const char *)sqlite3_column_text(stmt, 2));

		c->table = table_index(a, table);
		c->kind  = strcmp(type, "index") == 0 ? C_INDEX : C_TABLE;

		for (size_t x = 0; table && c->kind == C_INDEX && x < table->indexes.count; ++x) {
			if (strcmp(table->indexes.items[x].name, name) == 0) {
				c->index = &table->indexes.items[x];
			}
		}
	}

	sqlite3_finalize(stmt);
}

/* Cursors are what their opening instruction says, wherever the instruction stands. */
static void map_cursors(analysis *a) {
	for (size_t n = 0; n < a->ops.count; ++n) {
		const insn *i = &a->ops.items[n];
		cursor     *c;

		a->at = n;

		if (is(i, "OpenRead") || is(i, "OpenWrite") || is(i, "ReopenIdx")) {
			open_cursor(a, i);
		} else if (is(i, "SorterOpen") || is(i, "OpenEphemeral") || is(i, "OpenAutoindex")) {
			opened(a, i->p1)->kind = C_RECORDS;
		} else if (is(i, "OpenPseudo")) {
			c       = opened(a, i->p1);
			c->kind = C_PSEUDO;
			c->reg  = i->p2;
		} else if (is(i, "OpenDup")) {
			cursor *from = cursor_at(a, i->p2);
			int     kind = from ? from->kind : C_NONE;
			int     tab  = from ? from->table : -1;

			c        = opened(a, i->p1);
			c->kind  = kind;
			c->table = tab;
		}
	}

	for (size_t n = 0; n < a->ops.count; ++n) {
		const insn *i = &a->ops.items[n];
		cursor     *c;

		a->at = n;

		if (is(i, "NullRow")) {
			/* A left join's inner side: any of its reads can be the null row. */
			if ((c = cursor_at(a, i->p1)) != NULL) {
				c->null_row = true;
			}
		}
	}
}

unsigned sg_bytecode_column_types(const sg_column *column) {
	unsigned types;

	switch (column->affinity) {
	case SG_AFF_INTEGER: types = SG_BT_INT; break;
	case SG_AFF_REAL:    types = SG_BT_REAL; break;
	case SG_AFF_TEXT:    types = SG_BT_TEXT; break;
	case SG_AFF_NUMERIC: types = SG_BT_INT | SG_BT_REAL; break;
	case SG_AFF_BLOB:    types = column->declared_type[0] ? SG_BT_BLOB : SG_BT_UNKNOWN; break;
	default:             types = SG_BT_UNKNOWN; break;
	}

	return column->notnull ? types : types | SG_BT_NULL;
}

/* A table cursor's column number counts stored columns: a WITHOUT ROWID table stores its key
 * first, and nobody stores a virtual generated column. */
static int stored_to_column(const sg_table *t, long long stored) {
	long long at = 0;

	if (t->without_rowid) {
		for (int pos = 1; pos <= (int)t->columns.count; ++pos) {
			for (size_t c = 0; c < t->columns.count; ++c) {
				if (t->columns.items[c].pk == pos && at++ == stored) {
					return (int)c;
				}
			}
		}
	}

	for (size_t c = 0; c < t->columns.count; ++c) {
		const sg_column *col = &t->columns.items[c];

		if (col->generated == SG_GEN_VIRTUAL || (t->without_rowid && col->pk)) {
			continue;
		}

		if (at++ == stored) {
			return (int)c;
		}
	}

	return -2;
}

/* An index cursor's column: its key columns, then the table's rowid or primary key. */
static int index_to_column(const sg_table *t, const sg_index *x, long long n) {
	if (n < (long long)x->columns.count) {
		sg_column *c = sg_table_column(t, x->columns.items[n]);

		return c ? (int)(c - t->columns.items) : -2;
	}

	n -= (long long)x->columns.count;

	if (!t->without_rowid) {
		return n == 0 ? t->rowid_alias : -2;
	}

	for (int pos = 1; pos <= (int)t->columns.count; ++pos) {
		for (size_t c = 0; c < t->columns.count; ++c) {
			if (t->columns.items[c].pk == pos && n-- == 0) {
				return (int)c;
			}
		}
	}

	return -2;
}

/* ---- results of functions ------------------------------------------------------------------ */

static unsigned nulls(const aval *args, int count) {
	unsigned any = 0;

	for (int i = 0; i < count; ++i) {
		any |= args[i].types & (NULLISH | SG_BT_UNKNOWN);
	}

	return any ? SG_BT_NULL : 0;
}

static bool named(const char *name, const char *const *list) {
	for (size_t i = 0; list[i]; ++i) {
		if (strcmp(name, list[i]) == 0) {
			return true;
		}
	}

	return false;
}

/* What a built-in scalar function returns; SG_BT_UNKNOWN for any other. */
static unsigned scalar(const char *name, const aval *args, int count) {
	static const char *const int_of_args[]  = { "length", "octet_length", "instr", "unicode", "glob", "like", NULL };
	static const char *const int_always[]   = { "random", "changes", "total_changes", "last_insert_rowid", NULL };
	static const char *const text_of_args[] = { "lower", "upper", "trim", "ltrim", "rtrim", "replace", "substr",
			"substring", "printf", "format", "soundex", "unistr", "concat_ws", NULL };
	static const char *const text_always[]  = { "hex", "quote", "char", "concat", "typeof", "sqlite_version",
			"sqlite_source_id", NULL };
	static const char *const nullable_text[] = { "date", "time", "datetime", "strftime", "timediff", "json",
			"json_array", "json_object", "json_extract", "json_insert", "json_replace", "json_set", "json_remove",
			"json_patch", "json_quote", "json_type", NULL };
	static const char *const passing[]      = { "likelihood", "likely", "unlikely", NULL };

	if (named(name, int_of_args)) {
		return SG_BT_INT | nulls(args, count);
	}

	if (named(name, int_always)) {
		return SG_BT_INT;
	}

	if (named(name, text_of_args)) {
		return SG_BT_TEXT | nulls(args, count);
	}

	if (named(name, text_always)) {
		return SG_BT_TEXT;
	}

	if (named(name, nullable_text)) {
		return SG_BT_TEXT | SG_BT_NULL;
	}

	if (strcmp(name, "julianday") == 0) {
		return SG_BT_REAL | SG_BT_NULL;
	}

	if (strcmp(name, "unixepoch") == 0) {
		return SG_BT_INT | SG_BT_REAL | SG_BT_NULL;
	}

	if (strcmp(name, "round") == 0) {
		return SG_BT_REAL | nulls(args, count);
	}

	if (strcmp(name, "zeroblob") == 0 || strcmp(name, "randomblob") == 0) {
		return SG_BT_BLOB;
	}

	if (strcmp(name, "sign") == 0) {
		return SG_BT_INT | SG_BT_NULL;
	}

	if (strcmp(name, "abs") == 0 && count == 1) {
		unsigned t = args[0].types & (SG_BT_INT | SG_BT_REAL | SG_BT_NULL);

		return args[0].types & (SG_BT_TEXT | SG_BT_BLOB | SG_BT_UNKNOWN) ? t | SG_BT_REAL : t;
	}

	if (named(name, passing) && count >= 1) {
		return args[0].types;
	}

	if (strcmp(name, "nullif") == 0 && count == 2) {
		return args[0].types | SG_BT_NULL;
	}

	if ((strcmp(name, "min") == 0 || strcmp(name, "max") == 0) && count >= 2) {
		unsigned t = 0;

		for (int i = 0; i < count; ++i) {
			t |= args[i].types;
		}

		return t;
	}

	if ((strcmp(name, "coalesce") == 0 || strcmp(name, "ifnull") == 0) && count >= 1) {
		unsigned t    = 0;
		bool     some = false;

		for (int i = 0; i < count; ++i) {
			t   |= args[i].types & ~(unsigned)SG_BT_NULL;
			some = some || !(args[i].types & SG_BT_NULL);
		}

		return some ? t : t | SG_BT_NULL;
	}

	return SG_BT_UNKNOWN;
}

/* What a built-in aggregate returns, given its first argument; SG_BT_UNKNOWN for any other. */
static unsigned aggregate(const char *name, const aval *arg) {
	unsigned a = arg ? arg->types : SG_BT_UNKNOWN;

	if (strcmp(name, "count") == 0) {
		return SG_BT_INT;
	}

	if (strcmp(name, "sum") == 0) {
		return SG_BT_INT | SG_BT_REAL | SG_BT_NULL;
	}

	if (strcmp(name, "total") == 0) {
		return SG_BT_REAL;
	}

	if (strcmp(name, "avg") == 0) {
		return SG_BT_REAL | SG_BT_NULL;
	}

	if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
		return a | SG_BT_NULL;
	}

	if (strcmp(name, "group_concat") == 0 || strcmp(name, "string_agg") == 0) {
		return SG_BT_TEXT | SG_BT_NULL;
	}

	if (strcmp(name, "json_group_array") == 0 || strcmp(name, "json_group_object") == 0) {
		return SG_BT_TEXT;
	}

	return SG_BT_UNKNOWN;
}

/* "count(0)" -> "count" */
static void function_name(const insn *i, char *out, size_t size) {
	const char *p4  = i->p4 ? i->p4 : "";
	size_t      len = strcspn(p4, "(");

	snprintf(out, size, "%.*s", (int)(len < size ? len : size - 1), p4);
}

/* ---- parameters ---------------------------------------------------------------------------- */

/* The child column is a one-column foreign key to the parent column: both hold the same value. */
static bool references(const sg_table *child, int ccol, const sg_table *parent, int pcol) {
	if (ccol < 0 || pcol < 0) {
		return false;
	}

	for (size_t i = 0; i < child->foreign_keys.count; ++i) {
		const sg_foreign_key *fk = &child->foreign_keys.items[i];

		if (fk->columns.count == 1 && fk->resolved.count == 1
				&& sg_strcasecmp(fk->columns.items[0], child->columns.items[ccol].name) == 0
				&& sg_strcasecmp(fk->parent, parent->name) == 0
				&& sg_strcasecmp(fk->resolved.items[0], parent->columns.items[pcol].name) == 0) {
			return true;
		}
	}

	return false;
}

/* A parameter met a column. Meeting a foreign key and the key it references is meeting one value --
 * a join on them lets SQLite carry `child = ?` over to `parent = ?` -- and the child names it. */
static void link_param(analysis *a, const aval *v, int table, int column, unsigned types) {
	if (!v->param || v->param > (int)a->params.count) {
		return;
	}

	param_info *p = &a->params.items[v->param - 1];
	unsigned    t = types & ~(unsigned)SG_BT_NULL;

	if ((p->info.types | t) != p->info.types) {
		p->info.types |= t;
		a->changed = true;
	}

	if (table < 0) {
		return;
	}

	const sg_table *tab = &a->model->tables.items[table];

	if (!p->linked) {
		p->linked              = true;
		p->info.source.table   = tab;
		p->info.source.column  = column;
		a->changed             = true;
	} else if (p->info.several || (p->info.source.table == tab && p->info.source.column == column)
			|| references(p->info.source.table, p->info.source.column, tab, column)) {
		return;
	} else if (references(tab, column, p->info.source.table, p->info.source.column)) {
		p->info.source.table  = tab;
		p->info.source.column = column;
		a->changed            = true;
	} else {
		p->info.several = true;
		a->changed      = true;
	}
}

static unsigned affinity_types(int affinity) {
	switch (affinity) {
	case 'B': return SG_BT_TEXT;
	case 'C': return SG_BT_INT | SG_BT_REAL;
	case 'D': return SG_BT_INT;
	case 'E': return SG_BT_REAL;
	case 'A': return SG_BT_BLOB;
	default:  return 0;
	}
}

/* ---- transfer ------------------------------------------------------------------------------ */

static aval *reg(aval *state, const analysis *a, long long r) {
	static aval sink;

	if (r < 0 || (size_t)r >= a->nreg) {
		sink = value(SG_BT_UNKNOWN);
		return &sink;
	}

	return &state[r];
}

static void set(aval *state, const analysis *a, long long r, aval v) {
	if (r >= 0 && (size_t)r < a->nreg) {
		state[r] = v;
	}
}

/* The value of column @p n of cursor @p c, as the cursor stands in @p state. */
static aval read_column(analysis *a, aval *state, long long cn, long long n) {
	cursor *c = cursor_at(a, cn);

	if (!c) {
		return value(SG_BT_UNKNOWN | SG_BT_NULL);
	}

	if (c->kind == C_TABLE || c->kind == C_INDEX) {
		const sg_table *t   = c->table >= 0 ? &a->model->tables.items[c->table] : NULL;
		int             col = !t ? -2 : c->kind == C_TABLE ? stored_to_column(t, n)
				: c->index ? index_to_column(t, c->index, n) : -2;
		aval            v;

		if (col == -2) {
			return value(SG_BT_UNKNOWN | SG_BT_NULL);
		}

		v        = value(col >= 0 ? sg_bytecode_column_types(&t->columns.items[col]) : SG_BT_INT);
		v.table  = c->table;
		v.column = col;
		v.types |= c->null_row ? SG_BT_NULL : 0;
		return v;
	}

	const aval *record = c->kind == C_PSEUDO ? reg(state, a, c->reg) : NULL;
	long long   source = c->kind == C_RECORDS ? cn : -1;
	aval        out    = value(0);

	if (record && record->kind == K_RECORD) {
		aval *elements = a->records[record->ref];

		return elements ? elements[n] : value(SG_BT_UNKNOWN | SG_BT_NULL);
	}

	if (record && record->kind == K_CURSOR_RECORD) {
		source = record->ref;
	}

	cursor *holder = source >= 0 ? cursor_at(a, source) : NULL;

	for (size_t i = 0; holder && i < holder->records.count; ++i) {
		const insn *make = &a->ops.items[holder->records.items[i]];

		if (n < make->p2 && a->records[holder->records.items[i]]) {
			out = join(&out, &a->records[holder->records.items[i]][n]);
		}
	}

	return out.types ? out : value(SG_BT_UNKNOWN | SG_BT_NULL);
}

static void insert_record(analysis *a, long long cn, const aval *record) {
	cursor *c = cursor_at(a, cn);

	if (!c || record->kind != K_RECORD) {
		return;
	}

	for (size_t i = 0; i < c->records.count; ++i) {
		if (c->records.items[i] == record->ref) {
			return;
		}
	}

	*SG_PUSH(c->records) = record->ref;
	a->changed = true;
}

/* A row written into a table: each stored value lands in a column, which names its parameter. */
static void write_row(analysis *a, aval *state, long long cn, const aval *record, const aval *rowid) {
	cursor *c = cursor_at(a, cn);

	if (!c || c->kind != C_TABLE || c->table < 0) {
		insert_record(a, cn, record);
		return;
	}

	const sg_table *t = &a->model->tables.items[c->table];

	if (record->kind == K_RECORD && a->records[record->ref]) {
		const insn *make = &a->ops.items[record->ref];

		for (long long n = 0; n < make->p2; ++n) {
			int col = stored_to_column(t, n);

			if (col >= 0) {
				link_param(a, &a->records[record->ref][n], c->table, col,
						sg_bytecode_column_types(&t->columns.items[col]));
			}
		}
	}

	if (rowid && t->rowid_alias >= 0) {
		link_param(a, rowid, c->table, t->rowid_alias, SG_BT_INT);
	}

	(void)state;
}

static void seek_keys(analysis *a, aval *state, long long cn, long long first, long long count) {
	cursor *c = cursor_at(a, cn);

	if (!c || c->table < 0) {
		return;
	}

	const sg_table *t = &a->model->tables.items[c->table];

	for (long long k = 0; k < count; ++k) {
		int col = c->kind == C_INDEX && c->index ? index_to_column(t, c->index, k)
				: c->kind == C_TABLE && t->without_rowid ? stored_to_column(t, k) : -2;

		if (col >= 0) {
			link_param(a, reg(state, a, first + k), c->table, col,
					sg_bytecode_column_types(&t->columns.items[col]));
		}
	}
}

static void record_result(analysis *a, aval *state, long long first, long long count) {
	while (a->results.count < (size_t)count) {
		*SG_PUSH(a->results) = value(0);
	}

	for (long long n = 0; n < count; ++n) {
		aval joined = join(&a->results.items[n], reg(state, a, first + n));

		if (!same(&joined, &a->results.items[n])) {
			a->results.items[n] = joined;
			a->changed          = true;
		}
	}
}

/* "r[7]=…" or "r[6..9]=…" in an instruction's comment: what it writes, when it is not modelled. */
static void comment_writes(const analysis *a, const insn *i, aval *state) {
	for (const char *c = i->comment ? i->comment : ""; (c = strstr(c, "r[")) != NULL; ++c) {
		char     *end  = NULL;
		long long from = strtoll(c + 2, &end, 10);
		long long to   = from;

		if (end && strncmp(end, "..", 2) == 0) {
			to = strtoll(end + 2, &end, 10);
		}

		if (!end || strncmp(end, "]=", 2) != 0) {
			continue;
		}

		for (long long r = from; r <= to; ++r) {
			set(state, a, r, value(SG_BT_UNKNOWN | SG_BT_NULL));
		}
	}
}

static unsigned arithmetic(const aval *x, const aval *y, bool division) {
	unsigned nul = (x->types | y->types) & (NULLISH | SG_BT_UNKNOWN) ? SG_BT_NULL : 0;
	unsigned t   = (x->types | y->types) & SG_BT_REAL ? SG_BT_REAL : SG_BT_INT | SG_BT_REAL;

	return t | nul | (division ? SG_BT_NULL : 0);
}

static bool any_of(const insn *i, const char *const *ops) {
	return named(i->op, ops);
}

static const char *const COMPARISONS[] = { "Eq", "Ne", "Lt", "Le", "Gt", "Ge", NULL };
static const char *const SEEKS[] = { "SeekGE", "SeekGT", "SeekLE", "SeekLT", "IdxGE", "IdxGT", "IdxLE",
		"IdxLT", "Found", "NotFound", "NoConflict", "IfNoHope", NULL };

/* Control instructions, and those whose writes are none of the analysis's concern. */
static const char *const QUIET[] = { "Init", "Goto", "Halt", "Transaction", "TableLock", "Noop",
		"Explain", "Trace", "Once", "If", "IfNot", "IsNull", "NotNull", "Eq", "Ne", "Lt", "Le", "Gt",
		"Ge", "ElseEq", "Jump", "Rewind", "Next", "Prev", "SorterSort", "Sort", "SorterNext", "Last",
		"SeekGE", "SeekGT", "SeekLE", "SeekLT", "IdxGE", "IdxGT", "IdxLE", "IdxLT", "NotFound", "Found",
		"NoConflict", "NotExists", "SeekRowid", "SeekScan", "SeekHit", "IfNoHope", "IfSizeBetween",
		"OpenRead", "OpenWrite", "ReopenIdx", "OpenEphemeral", "OpenAutoindex", "OpenDup", "SorterOpen",
		"OpenPseudo", "Close", "ColumnsUsed", "Delete", "IdxDelete", "ResetCount", "ResultRow",
		"FkCounter", "FkIfZero", "Affinity", "TypeCheck", "CursorHint", "NullRow", "DeferredSeek",
		"FinishSeek", "Insert", "IdxInsert", "SorterInsert", "Return", "EndCoroutine", "MakeRecord",
		"IsType", "Filter", "FilterAdd", "Compare", "Permutation", "HaltIfNull", "MustBeInt",
		"RealAffinity", "Expire", "CursorLock", "CursorUnlock", NULL };

static void transfer(analysis *a, size_t at, aval *s) {
	const insn *i = &a->ops.items[at];
	char        fn[64];

	a->at = at;

	if (is(i, "Integer")) {
		aval v = value(SG_BT_INT);

		v.has_constant = true;
		v.constant     = i->p1;
		set(s, a, i->p2, v);
	} else if (is(i, "Int64")) {
		set(s, a, i->p2, value(SG_BT_INT));
	} else if (is(i, "Real")) {
		set(s, a, i->p2, value(SG_BT_REAL));
	} else if (is(i, "String8") || is(i, "String")) {
		set(s, a, i->p2, value(SG_BT_TEXT));
	} else if (is(i, "Blob")) {
		set(s, a, i->p2, value(SG_BT_BLOB));
	} else if (is(i, "Null")) {
		for (long long r = i->p2; r <= (i->p3 > i->p2 ? i->p3 : i->p2); ++r) {
			set(s, a, r, value(cleared(a, at)));
		}
	} else if (is(i, "SoftNull")) {
		set(s, a, i->p1, value(cleared(a, at)));
	} else if (is(i, "BeginSubrtn")) {
		set(s, a, i->p2, value(cleared(a, at)));
	} else if (is(i, "Variable")) {
		aval v = value(SG_BT_UNKNOWN | SG_BT_NULL);

		v.param = (int)i->p1;
		set(s, a, i->p2, v);
	} else if (is(i, "Copy")) {
		for (long long n = 0; n <= i->p3; ++n) {
			set(s, a, i->p2 + n, *reg(s, a, i->p1 + n));
		}
	} else if (is(i, "SCopy")) {
		set(s, a, i->p2, *reg(s, a, i->p1));
	} else if (is(i, "IntCopy")) {
		set(s, a, i->p2, value(SG_BT_INT));
	} else if (is(i, "Move")) {
		for (long long n = 0; n < i->p3; ++n) {
			set(s, a, i->p2 + n, *reg(s, a, i->p1 + n));
			set(s, a, i->p1 + n, value(cleared(a, at)));
		}
	} else if (is(i, "Column")) {
		set(s, a, i->p3, read_column(a, s, i->p1, i->p2));
	} else if (is(i, "Rowid") || is(i, "IdxRowid")) {
		cursor *c = cursor_at(a, i->p1);
		aval    v = value(SG_BT_INT | (c && c->null_row ? SG_BT_NULL : 0));

		if (c && c->table >= 0) {
			v.table  = c->table;
			v.column = a->model->tables.items[c->table].rowid_alias;
		}

		set(s, a, i->p2, v);
	} else if (is(i, "Sequence") || is(i, "NewRowid") || is(i, "Count") || is(i, "AddImm")) {
		set(s, a, is(i, "AddImm") ? i->p1 : i->p2, value(SG_BT_INT));
	} else if (is(i, "Function") || is(i, "PureFunc")) {
		const char *open  = i->p4 ? strchr(i->p4, '(') : NULL;
		int         nargs = open ? atoi(open + 1) : 0;
		aval        args[16];

		/* A function taking any number of arguments is listed as name(-1); its comment names the
		 * argument registers: func(r[4..5]). */
		if (nargs < 0) {
			const char *args_at = i->comment ? strstr(i->comment, "func(r[") : NULL;
			char       *end     = NULL;
			long long   from    = args_at ? strtoll(args_at + 7, &end, 10) : 0;
			long long   to      = end && strncmp(end, "..", 2) == 0 ? strtoll(end + 2, NULL, 10) : from;

			nargs = args_at ? (int)(to - from + 1) : 0;
		}

		nargs = nargs < 0 ? 0 : nargs > 16 ? 16 : nargs;

		for (int n = 0; n < nargs; ++n) {
			args[n] = *reg(s, a, i->p2 + n);
		}

		function_name(i, fn, sizeof fn);
		set(s, a, i->p3, value(nargs == 16 ? SG_BT_UNKNOWN : scalar(fn, args, nargs)));

		/* A parameter given to a function taking text is text. */
		static const char *const takes_text[] = { "like", "glob", "upper", "lower", "trim", "ltrim", "rtrim",
				"replace", "instr", "soundex", "json", "json_extract", "json_type", "json_array_length",
				"json_valid", "json_patch", "json_set", "json_insert", "json_replace", "json_remove", NULL };

		for (int n = 0; named(fn, takes_text) && n < nargs; ++n) {
			link_param(a, &args[n], -1, -1, SG_BT_TEXT);
		}
	} else if (is(i, "AggStep") || is(i, "AggStep1") || is(i, "AggInverse")) {
		if (i->p3 >= 0 && (size_t)i->p3 < a->nreg) {
			aval *first  = reg(s, a, i->p2);
			aval  joined = a->acc[i->p3] ? join(a->acc[i->p3], first) : *first;

			if (!a->acc[i->p3]) {
				a->acc[i->p3]  = sg_xmalloc(sizeof(aval));
				*a->acc[i->p3] = value(0);
			}

			if (!same(&joined, a->acc[i->p3])) {
				*a->acc[i->p3] = joined;
				a->changed     = true;
			}
		}
	} else if (is(i, "AggFinal") || is(i, "AggValue")) {
		long long acc  = i->p1;
		long long dest = is(i, "AggFinal") ? i->p1 : i->p3;

		function_name(i, fn, sizeof fn);
		set(s, a, dest, value(aggregate(fn, acc >= 0 && (size_t)acc < a->nreg ? a->acc[acc] : NULL)));
	} else if (is(i, "Add") || is(i, "Subtract") || is(i, "Multiply") || is(i, "Divide") || is(i, "Remainder")) {
		bool div = is(i, "Divide") || is(i, "Remainder");

		link_param(a, reg(s, a, i->p1), -1, -1, SG_BT_INT | SG_BT_REAL);
		link_param(a, reg(s, a, i->p2), -1, -1, SG_BT_INT | SG_BT_REAL);
		set(s, a, i->p3, value(arithmetic(reg(s, a, i->p1), reg(s, a, i->p2), div)));
	} else if (is(i, "Concat")) {
		unsigned nul = (reg(s, a, i->p1)->types | reg(s, a, i->p2)->types) & (NULLISH | SG_BT_UNKNOWN);

		set(s, a, i->p3, value(SG_BT_TEXT | (nul ? SG_BT_NULL : 0)));
	} else if (is(i, "BitAnd") || is(i, "BitOr") || is(i, "ShiftLeft") || is(i, "ShiftRight")) {
		unsigned nul = (reg(s, a, i->p1)->types | reg(s, a, i->p2)->types) & (NULLISH | SG_BT_UNKNOWN);

		set(s, a, i->p3, value(SG_BT_INT | (nul ? SG_BT_NULL : 0)));
	} else if (is(i, "BitNot") || is(i, "Not")) {
		unsigned nul = reg(s, a, i->p1)->types & (NULLISH | SG_BT_UNKNOWN);

		set(s, a, i->p2, value(SG_BT_INT | (nul ? SG_BT_NULL : 0)));
	} else if (is(i, "IsTrue")) {
		set(s, a, i->p2, value(SG_BT_INT));
	} else if (is(i, "ZeroOrNull")) {
		set(s, a, i->p2, value(SG_BT_INT | SG_BT_NULL));
	} else if (is(i, "CollSeq")) {
		if (i->p1) {
			set(s, a, i->p1, value(SG_BT_INT));
		}
	} else if (is(i, "Cast")) {
		aval    *v   = reg(s, a, i->p1);
		unsigned nul = v->types & NULLISH;
		unsigned t   = affinity_types((int)i->p2);
		aval     out = *v;

		out.types  = (t ? t : SG_BT_UNKNOWN) | nul;
		out.kind   = K_VALUE;
		out.table  = -1;
		out.column = -1;
		set(s, a, i->p1, out);
	} else if (is(i, "MakeRecord")) {
		aval *elements = a->records[at];

		if (!elements) {
			elements       = sg_xmalloc(sizeof(aval) * (size_t)(i->p2 > 0 ? i->p2 : 1));
			a->records[at] = elements;

			for (long long n = 0; n < i->p2; ++n) {
				elements[n] = value(0);
			}
		}

		for (long long n = 0; n < i->p2; ++n) {
			aval *in     = reg(s, a, i->p1 + n);
			aval  joined = join(&elements[n], in);

			if (i->p4 && (long long)strlen(i->p4) > n) {
				link_param(a, in, -1, -1, affinity_types(i->p4[n]));
			}

			if (!same(&joined, &elements[n])) {
				elements[n] = joined;
				a->changed  = true;
			}
		}

		aval record = value(SG_BT_BLOB);

		record.kind = K_RECORD;
		record.ref  = (long long)at;
		set(s, a, i->p3, record);
	} else if (is(i, "SorterData") || is(i, "RowData")) {
		aval record = value(SG_BT_BLOB);

		record.kind = K_CURSOR_RECORD;
		record.ref  = i->p1;
		set(s, a, i->p2, record);
	} else if (is(i, "Gosub") || is(i, "InitCoroutine") || is(i, "Yield")) {
		aval addr = value(SG_BT_INT);

		addr.kind = K_ADDRESS;
		set(s, a, i->p1, addr);
	} else if (is(i, "IfPos") || is(i, "IfNotZero") || is(i, "DecrJumpZero")) {
		set(s, a, i->p1, value(SG_BT_INT));
	} else if (!any_of(i, QUIET)) {
		comment_writes(a, i, s);
	}

	/* What instructions tell about parameters, and what they do with records. */
	if (is(i, "Affinity")) {
		for (long long n = 0; i->p4 && n < i->p2 && n < (long long)strlen(i->p4); ++n) {
			link_param(a, reg(s, a, i->p1 + n), -1, -1, affinity_types(i->p4[n]));
		}
	} else if (any_of(i, COMPARISONS)) {
		aval *x = reg(s, a, i->p1);
		aval *y = reg(s, a, i->p3);

		link_param(a, x, y->table, y->column, y->types);
		link_param(a, y, x->table, x->column, x->types);
	} else if (any_of(i, SEEKS) && p4_int(i) > 0) {
		seek_keys(a, s, i->p1, i->p3, p4_int(i));
	} else if (is(i, "SeekRowid") || is(i, "NotExists")) {
		cursor *c = cursor_at(a, i->p1);

		if (c && c->table >= 0 && a->model->tables.items[c->table].rowid_alias >= 0) {
			link_param(a, reg(s, a, i->p3), c->table, a->model->tables.items[c->table].rowid_alias, SG_BT_INT);
		} else {
			link_param(a, reg(s, a, i->p3), -1, -1, SG_BT_INT);
		}
	} else if (is(i, "SorterInsert") || is(i, "IdxInsert")) {
		insert_record(a, i->p1, reg(s, a, i->p2));
	} else if (is(i, "Insert")) {
		write_row(a, s, i->p1, reg(s, a, i->p2), reg(s, a, i->p3));
	} else if (is(i, "ResultRow")) {
		record_result(a, s, i->p1, i->p2);
	}
}

/* ---- control flow ------------------------------------------------------------------------- */

static const char *const JUMPS[] = { "If", "IfNot", "IsNull", "NotNull", "Eq", "Ne", "Lt", "Le", "Gt",
		"Ge", "ElseEq", "IfPos", "IfNotZero", "DecrJumpZero", "Once", "Rewind", "Next", "Prev",
		"SorterSort", "Sort", "SorterNext", "Last", "SeekGE", "SeekGT", "SeekLE", "SeekLT", "SeekRowid",
		"NotExists", "IdxGE", "IdxGT", "IdxLE", "IdxLT", "NotFound", "Found", "NoConflict", "IfNullRow",
		"MustBeInt", "IsType", "IfNoHope", "IfSizeBetween", "SeekScan", "FkIfZero", "Filter", "VFilter",
		"VNext", "RowSetRead", "RowSetTest", "IfEmpty", "SorterCompare", "IfSmaller", "IfNotOpen", NULL };

typedef struct {
	size_t   to;
	int      refine;     /* 0 none; 1 the register is NULL; 2 it is not; 3 nothing is cleared */
	long long reg;
} edge;

typedef SG_VEC(edge) edges;

static void add_edge(edges *out, const analysis *a, long long to, int refine, long long r) {
	if (to < 0 || (size_t)to >= a->ops.count) {
		return;
	}

	edge *e = SG_PUSH(*out);

	e->to     = (size_t)to;
	e->refine = refine;
	e->reg    = r;
}

/* Where a Return or Yield can go: after every instruction storing a return address in that register. */
static void returns_of(const analysis *a, long long r, bool yield, edges *out) {
	for (size_t n = 0; n < a->ops.count; ++n) {
		const insn *i = &a->ops.items[n];

		if (i->p1 != r) {
			continue;
		}

		if (is(i, "Gosub") && !yield) {
			add_edge(out, a, (long long)n + 1, 0, 0);
		} else if (is(i, "InitCoroutine") && yield) {
			add_edge(out, a, i->p3, 0, 0);
		} else if (is(i, "Yield") && yield) {
			add_edge(out, a, (long long)n + 1, 0, 0);
		}
	}
}

static void successors(const analysis *a, size_t at, const aval *state, edges *out) {
	const insn *i = &a->ops.items[at];

	out->count = 0;

	if (is(i, "Halt")) {
		return;
	}

	if (is(i, "Goto") || is(i, "Init")) {
		add_edge(out, a, i->p2, 0, 0);
	} else if (is(i, "Gosub")) {
		add_edge(out, a, i->p2, 0, 0);
	} else if (is(i, "Return")) {
		returns_of(a, i->p1, false, out);

		if (i->p3 == 1) {
			add_edge(out, a, (long long)at + 1, 0, 0);
		}
	} else if (is(i, "InitCoroutine")) {
		add_edge(out, a, i->p2 ? i->p2 : (long long)at + 1, 0, 0);
	} else if (is(i, "Yield")) {
		returns_of(a, i->p1, true, out);
	} else if (is(i, "EndCoroutine")) {
		for (size_t n = 0; n < a->ops.count; ++n) {
			if (is(&a->ops.items[n], "Yield") && a->ops.items[n].p1 == i->p1) {
				add_edge(out, a, a->ops.items[n].p2, 0, 0);
			}
		}
	} else if (is(i, "Jump")) {
		add_edge(out, a, i->p1, 0, 0);
		add_edge(out, a, i->p2, 0, 0);
		add_edge(out, a, i->p3, 0, 0);
	} else if (is(i, "IsNull")) {
		add_edge(out, a, i->p2, 1, i->p1);
		add_edge(out, a, (long long)at + 1, 2, i->p1);
	} else if (is(i, "NotNull")) {
		add_edge(out, a, i->p2, 2, i->p1);
		add_edge(out, a, (long long)at + 1, 1, i->p1);
	} else if (is(i, "DecrJumpZero") && state && i->p1 >= 0 && (size_t)i->p1 < a->nreg
			&& state[i->p1].has_constant && state[i->p1].constant == 1) {
		/* A counter holding 1, as LIMIT 1 sets it, always reaches zero: the loop never goes round. */
		add_edge(out, a, i->p2, 0, 0);
	} else if (is(i, "IfPos") && i->p1 >= 0 && (size_t)i->p1 < a->nreg && a->flag[i->p1]) {
		/* The flag is set: what it guards was filled after it was cleared. */
		add_edge(out, a, i->p2, 3, i->p1);
		add_edge(out, a, (long long)at + 1, 0, 0);
	} else if (any_of(i, JUMPS)) {
		add_edge(out, a, i->p2, 0, 0);
		add_edge(out, a, (long long)at + 1, 0, 0);
	} else {
		if (!any_of(i, QUIET) && i->p2 > 0 && (size_t)i->p2 < a->ops.count && i->p2 != (long long)at + 1
				&& strstr(i->comment ? i->comment : "", "goto")) {
			add_edge(out, a, i->p2, 0, 0);
		}

		add_edge(out, a, (long long)at + 1, 0, 0);
	}
}

/* ---- the fixpoint -------------------------------------------------------------------------- */

static bool merge(aval *into, const aval *from, size_t n) {
	bool grew = false;

	for (size_t r = 0; r < n; ++r) {
		aval joined = join(&into[r], &from[r]);

		if (!same(&joined, &into[r])) {
			into[r] = joined;
			grew    = true;
		}
	}

	return grew;
}

static void run(analysis *a, aval *in, bool *reached) {
	size_t  n     = a->ops.count;
	aval   *state = sg_xmalloc(sizeof(aval) * a->nreg);
	edges   out   = {0};
	SG_VEC(size_t) work = {0};
	bool   *queued = sg_xmalloc(n ? n : 1);

	memset(queued, 0, n ? n : 1);

	for (size_t at = 0; at < n; ++at) {
		if (reached[at]) {
			*SG_PUSH(work) = at;
			queued[at]     = true;
		}
	}

	while (work.count) {
		size_t at = work.items[--work.count];

		queued[at] = false;
		memcpy(state, &in[at * a->nreg], sizeof(aval) * a->nreg);
		transfer(a, at, state);
		successors(a, at, &in[at * a->nreg], &out);

		for (size_t e = 0; e < out.count; ++e) {
			aval *target = &in[out.items[e].to * a->nreg];
			aval  saved  = value(0);
			long long r  = out.items[e].reg;
			bool  refine = (out.items[e].refine == 1 || out.items[e].refine == 2) && r >= 0 && (size_t)r < a->nreg;
			aval *kept   = NULL;

			if (out.items[e].refine == 3) {
				kept = sg_xmalloc(sizeof(aval) * a->nreg);
				memcpy(kept, state, sizeof(aval) * a->nreg);

				for (size_t k = 0; k < a->nreg; ++k) {
					if (state[k].types & CLEARED) {
						state[k].types &= ~(unsigned)CLEARED;
						state[k].types  = state[k].types ? state[k].types : SG_BT_NULL;
					}
				}
			}

			if (refine) {
				saved = state[r];

				/* Only the types narrow: the value is still that column's, or that parameter. */
				if (out.items[e].refine == 1) {
					state[r].types = SG_BT_NULL;
				} else {
					state[r].types &= ~(unsigned)NULLISH;
					state[r].types  = state[r].types ? state[r].types : SG_BT_UNKNOWN;
				}
			}

			bool grew = merge(target, state, a->nreg) || !reached[out.items[e].to];

			reached[out.items[e].to] = true;

			if (refine) {
				state[r] = saved;
			}

			if (kept) {
				memcpy(state, kept, sizeof(aval) * a->nreg);
				free(kept);
			}

			if (grew && !queued[out.items[e].to]) {
				*SG_PUSH(work) = out.items[e].to;
				queued[out.items[e].to] = true;
			}
		}
	}

	free(queued);
	free(work.items);
	free(out.items);
	free(state);
}

/* ---- rows ---------------------------------------------------------------------------------- */

/* Whether @p target can be reached from @p from, never passing @p avoid (when it is not -1). */
static bool reaches(const analysis *a, const aval *in, size_t from, size_t target, long long avoid,
		bool *seen) {
	SG_VEC(size_t) stack = {0};
	edges          out   = {0};
	bool           found = false;

	memset(seen, 0, a->ops.count);
	*SG_PUSH(stack) = from;

	while (stack.count && !found) {
		size_t at = stack.items[--stack.count];

		successors(a, at, in ? &in[at * a->nreg] : NULL, &out);

		for (size_t e = 0; e < out.count && !found; ++e) {
			size_t to = out.items[e].to;

			if ((long long)to == avoid) {
				continue;
			}

			if (to == target) {
				found = true;
			} else if (!seen[to]) {
				seen[to]        = true;
				*SG_PUSH(stack) = to;
			}
		}
	}

	free(stack.items);
	free(out.items);
	return found;
}

static sg_rows count_rows(const analysis *a, const aval *in, const bool *reached) {
	SG_VEC(size_t) rows = {0};
	bool          *seen = sg_xmalloc(a->ops.count ? a->ops.count : 1);
	sg_rows        result;

	for (size_t at = 0; at < a->ops.count; ++at) {
		if (reached[at] && is(&a->ops.items[at], "ResultRow")) {
			*SG_PUSH(rows) = at;
		}
	}

	if (rows.count == 0) {
		result = SG_ROWS_NONE;
	} else if (rows.count > 1 || reaches(a, in, rows.items[0], rows.items[0], -1, seen)) {
		result = SG_ROWS_MANY;
	} else {
		bool bypass = false;

		for (size_t at = 0; at < a->ops.count && !bypass; ++at) {
			if (reached[at] && is(&a->ops.items[at], "Halt")) {
				bypass = at == 0 || reaches(a, in, 0, at, (long long)rows.items[0], seen);
			}
		}

		result = bypass ? SG_ROWS_AT_MOST_ONE : SG_ROWS_EXACTLY_ONE;
	}

	free(seen);
	free(rows.items);
	return result;
}

/* ---- the whole --------------------------------------------------------------------------- */

static void free_analysis(analysis *a) {
	for (size_t n = 0; n < a->ops.count; ++n) {
		free(a->ops.items[n].p4);
		free(a->ops.items[n].comment);

		if (a->records) {
			free(a->records[n]);
		}
	}

	for (size_t r = 0; a->acc && r < a->nreg; ++r) {
		free(a->acc[r]);
	}

	for (size_t c = 0; c < a->cursors.count; ++c) {
		free(a->cursors.items[c].records.items);
	}

	free(a->ops.items);
	free(a->flag);
	free(a->clears);
	free(a->records);
	free(a->acc);
	free(a->cursors.items);
	free(a->params.items);
	free(a->results.items);
}

bool sg_bytecode_analyze(sqlite3 *db, const sg_model *model, const char *sql, const char *file,
		sg_banalysis *out) {
	analysis      a;
	sqlite3_stmt *stmt = NULL;

	memset(&a, 0, sizeof a);
	memset(out, 0, sizeof *out);
	a.db    = db;
	a.model = model;
	a.file  = file;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		sg_error(file, 0, 0, "%s", sqlite3_errmsg(db));
		return false;
	}

	int nparams = sqlite3_bind_parameter_count(stmt);

	sqlite3_finalize(stmt);

	for (int p = 0; p < nparams; ++p) {
		SG_PUSH(a.params)->info.source.column = -1;
	}

	/* Foreign key checks look the parent row up with the child's value, so a parameter stored into
	 * the child column would also seem compared with the parent key; they say nothing about types. */
	int  enforced = 0;
	bool read;

	sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, -1, &enforced);
	sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, 0, NULL);
	read = read_program(&a, sql);
	sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, enforced, NULL);

	if (!read || a.ops.count == 0) {
		free_analysis(&a);
		return false;
	}

	map_cursors(&a);

	a.flag   = sg_xmalloc(a.nreg);
	a.clears = sg_xmalloc(a.ops.count);
	memset(a.flag, 0, a.nreg);
	memset(a.clears, 0, a.ops.count);
	find_flags(&a);
	a.records = sg_xmalloc(sizeof(aval *) * a.ops.count);
	a.acc     = sg_xmalloc(sizeof(aval *) * a.nreg);
	memset(a.records, 0, sizeof(aval *) * a.ops.count);
	memset(a.acc, 0, sizeof(aval *) * a.nreg);

	aval *in      = sg_xmalloc(sizeof(aval) * a.nreg * a.ops.count);
	bool *reached = sg_xmalloc(a.ops.count);

	for (size_t k = 0; k < a.nreg * a.ops.count; ++k) {
		in[k] = value(0);
	}

	memset(reached, 0, a.ops.count);
	reached[0] = true;

	/* Registers start out NULL. */
	for (size_t r = 0; r < a.nreg; ++r) {
		in[r] = value(cleared(&a, 0));
	}

	/* What one pass learns about records, aggregates and parameters can change what another reads. */
	int passes = 0;

	do {
		a.changed = false;
		run(&a, in, reached);
	} while (a.changed && ++passes < MAX_PASSES);

	for (size_t n = 0; n < a.results.count; ++n) {
		sg_bresult *r = SG_PUSH(out->results);
		const aval *v = &a.results.items[n];

		unsigned t = v->types ? v->types : SG_BT_UNKNOWN;

		/* A cleared NULL no flag ruled out is a NULL like any other. */
		if (t & CLEARED) {
			t = (t & ~(unsigned)CLEARED) | SG_BT_NULL;
		}

		r->types         = t;
		r->source.table  = v->table >= 0 ? &model->tables.items[v->table] : NULL;
		r->source.column = v->column;
	}

	for (size_t p = 0; p < a.params.count; ++p) {
		*SG_PUSH(out->params) = a.params.items[p].info;
	}

	out->rows = count_rows(&a, in, reached);

	free(in);
	free(reached);
	free_analysis(&a);
	return true;
}

void sg_banalysis_free(sg_banalysis *result) {
	free(result->results.items);
	free(result->params.items);
	memset(result, 0, sizeof *result);
}
