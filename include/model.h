#ifndef SQLGEN_MODEL_H
#define SQLGEN_MODEL_H

/**
 * @file model.h
 * @brief The schema model: everything SQLite reports about a schema, plus what follows from it.
 *
 * Built by @ref sg_introspect from a database and written out by @ref sg_model_write_json.
 * Every string is owned by the model and released by @ref sg_model_free.
 */

#include <sqlite3.h>

#include "util.h"

typedef enum {
	SG_TABLE,
	SG_VIEW,
	SG_VIRTUAL
} sg_table_kind;

/** @brief SQLite's type affinity, from the declared type by the rules of datatype3 section 3.1. */
typedef enum {
	SG_AFF_INTEGER,
	SG_AFF_TEXT,
	SG_AFF_BLOB,      //!< Also an empty declared type: "BLOB affinity", formerly called NONE.
	SG_AFF_REAL,
	SG_AFF_NUMERIC,
	SG_AFF_ANY        //!< A STRICT table's ANY, which converts nothing.
} sg_affinity;

typedef enum {
	SG_GEN_NONE,
	SG_GEN_VIRTUAL,   //!< `hidden` = 2 in pragma_table_xinfo.
	SG_GEN_STORED     //!< `hidden` = 3.
} sg_generated;

/**
 * @brief Where a schema object was created. Absent for a model read from a live database.
 */
typedef struct {
	const char *script;   //!< The script path; borrowed from the patch set.
	const char *patch;    //!< The patch version text; NULL for the system-tables script.
	bool        system;   //!< Created by the system-tables script.
	unsigned    line;     //!< 1-based line of the statement that created it.
	unsigned    col;      //!< 1-based byte column of that statement.
} sg_origin;

typedef struct {
	char         *name;
	char         *declared_type;     //!< As SQLite reports it; "" when none was declared.
	sg_affinity   affinity;
	bool          notnull_declared;  //!< What `notnull` in pragma_table_xinfo says.
	bool          notnull;           //!< Whether NULL is actually impossible (see sg_introspect).
	char         *default_sql;       //!< The default expression without its outer parentheses; NULL when none.
	int           pk;                //!< 1-based position in the primary key, 0 when not part of it.
	sg_generated  generated;
	bool          hidden;            //!< A hidden column of a virtual table (`hidden` = 1).
	char         *collation;         //!< NULL for views.
	bool          autoincrement;
	bool          rowid_alias;       //!< This column is the table's rowid.
	bool          needs_quote;       //!< A keyword or not a plain identifier; must be quoted in SQL.
	char         *origin_table;      //!< Views only: the table the value comes from; NULL for an expression.
	char         *origin_column;
} sg_column;

typedef SG_VEC(char *) sg_names;

typedef struct {
	char       *name;
	bool        unique;
	char       *origin;      //!< "create", "unique" or "primary_key".
	bool        partial;
	bool        expression;  //!< At least one key part is an expression rather than a column.
	sg_names    columns;     //!< Key columns in order; "" for an expression part.
	SG_VEC(bool) descending;
	sg_names    collations;
	sg_origin   created;
} sg_index;

typedef struct {
	sg_names    columns;         //!< In the order of the primary key or index.
	char       *source;          //!< "rowid" or the index name.
	bool        total;           //!< Every column is NOT NULL, so the key names at most one row.
} sg_unique_key;

typedef struct {
	int         id;              //!< SQLite's id for the constraint.
	sg_names    columns;         //!< The child columns.
	char       *parent;
	sg_names    parent_columns;  //!< As declared; empty when the declaration omits them.
	sg_names    resolved;        //!< The parent columns actually referenced (its primary key when omitted).
	bool        parent_exists;
	char       *on_update;
	char       *on_delete;
	bool        indexed;         //!< Its columns lead some index of the child table.
} sg_foreign_key;

typedef struct {
	char                    *name;
	sg_table_kind            kind;
	bool                     without_rowid;
	bool                     strict;
	bool                     autoincrement;
	int                      rowid_alias;   //!< Index into columns, or -1.
	bool                     junction;
	sg_origin                created;
	SG_VEC(sg_column)        columns;
	SG_VEC(sg_index)         indexes;
	SG_VEC(sg_unique_key)    unique_keys;
	SG_VEC(sg_foreign_key)   foreign_keys;
	sg_names                 triggers;
} sg_table;

typedef struct {
	char               *sqlite_version;
	SG_VEC(sg_table)    tables;          //!< Tables, views and virtual tables, sorted by name.
} sg_model;

/**
 * @brief Read the schema of the main database of @p db into @p out.
 * @return false, with errors reported, when a query fails.
 *
 * Calls, per table: pragma_table_list, pragma_table_xinfo, sqlite3_table_column_metadata,
 * pragma_index_list, pragma_index_xinfo, pragma_foreign_key_list; for views, a prepared
 * `select *` read through sqlite3_column_table_name and sqlite3_column_origin_name; and
 * sqlite_schema for triggers.
 *
 * A column is taken as NOT NULL when it is declared so, when it is the rowid alias, or when it is
 * part of the primary key of a WITHOUT ROWID table. Any other primary-key column of a rowid table
 * can hold NULL, which SQLite accepts for compatibility. (A STRICT table already reports its
 * primary-key columns as declared NOT NULL.)
 */
bool sg_introspect(sqlite3 *db, sg_model *out);

/** @brief Find a table by name, ASCII case-insensitively as SQLite matches identifiers; NULL when there is none. */
sg_table *sg_model_table(const sg_model *model, const char *name);

/** @brief Find a column by name, case-insensitively like @ref sg_model_table; NULL when there is none. */
sg_column *sg_table_column(const sg_table *table, const char *name);

/** @brief The affinity's name as SQLite's documentation spells it, lowercase. */
const char *sg_affinity_name(sg_affinity affinity);

/** @brief Release everything. */
void sg_model_free(sg_model *model);

/**
 * @brief Write @p model as JSON.
 * @param patches  The loaded patch set, or NULL for a model read from a live database.
 * @param database The database file, when @p patches is NULL.
 */
struct sg_patchset;
void sg_model_write_json(const sg_model *model, const struct sg_patchset *patches,
		const char *database, sg_buf *out);

#endif /* SQLGEN_MODEL_H */
