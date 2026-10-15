#ifndef SQLGEN_PLAN_H
#define SQLGEN_PLAN_H

/**
 * @file plan.h
 * @brief The generation plan: everything to generate, decided once, for any language to spell.
 *
 * Built from the model and the configuration. It decides which types and statements exist, the
 * exact SQL of every statement and the order of its parameters, and the logical type of every
 * column -- so that every language runs the same SQL. It decides nothing about spelling: names are
 * kept as lists of lowercase words (`token_hash` and `tokenHash` are both `token hash`), and each
 * language joins them in its own style.
 *
 * Per table:
 *   - a row type, with one field per column;
 *   - a *get* per unique key whose columns are all NOT NULL -- it identifies at most one row; the
 *     primary key's is named `get by key`;
 *   - a *list* per leading prefix of every index that is not partial and holds no expression,
 *     unless that prefix is such a unique key; ordered by the primary key, or by rowid, so a list
 *     comes back in the same order every time;
 *   - the writes the table opts into: *insert*, *update* (by primary key), *delete* (by every
 *     get's and list's columns), *upsert* (on every unique key the insert binds), *link* and
 *     *unlink* (junctions only).
 * Views get the row type only, and virtual tables the row type only.
 *
 * Parameters are numbered `?1`, `?2`, … in the order @ref sg_statement::params lists them.
 * Comparing with `=`, a parameter bound to NULL matches no row.
 */

#include "config.h"
#include "model.h"
#include "query.h"
#include "schema.h"

typedef sg_names sg_words;

/** @brief Column positions within a table. */
typedef SG_VEC(size_t) sg_index_list;

typedef enum {
	SG_STMT_GET,
	SG_STMT_LIST,
	SG_STMT_INSERT,
	SG_STMT_UPDATE,
	SG_STMT_DELETE,
	SG_STMT_UPSERT,
	SG_STMT_LINK,
	SG_STMT_UNLINK
} sg_stmt_kind;

typedef struct {
	size_t column;      //!< Index into the table's columns.
	bool   optional;    //!< Insert only: may be left unbound, giving the column's default or a new rowid.
} sg_param;

typedef struct {
	sg_stmt_kind       kind;
	sg_words           words;     //!< "get by username", "list by family", "insert", …
	char              *sql;
	SG_VEC(sg_param)   params;    //!< In placeholder order.
	sg_index_list      key;       //!< The columns it selects rows by; empty for insert.
	sg_index_list      returning; //!< Insert into a WITHOUT ROWID table: the key columns it returns.
} sg_statement;

typedef struct {
	const sg_column *column;    //!< Borrowed from the model.
	sg_words         words;
	char            *type;      //!< Logical type: an affinity name, or a name from `types`.
	bool             custom;    //!< The type came from `types`, so every target must map it.
	bool             nullable;
	bool             writable;  //!< A table's column that is neither generated nor hidden.
	const char      *id_of;     //!< Holds that table's typed id (its own key, or a foreign key's); NULL otherwise.
	char            *sql_name;  //!< The name as SQL needs it: quoted when it is a keyword or not plain.
} sg_plan_column;

typedef struct {
	const sg_table           *table;     //!< Borrowed from the model.
	sg_words                  words;
	char                     *sql_name;
	SG_VEC(sg_plan_column)    columns;
	char                     *select_list;
	int                       id_column; //!< The column that is its typed id, or -1.
	SG_VEC(sg_statement)      statements;
} sg_plan_table;

typedef struct {
	char *name;
	char *path;
	char *text;
} sg_plan_script;

typedef struct {
	const sg_patch          *patch;     //!< Borrowed from the patch set.
	SG_VEC(sg_plan_script)   scripts;
} sg_plan_patch;

typedef struct {
	const sg_config          *config;
	SG_VEC(sg_plan_table)     tables;
	sg_plan_script            systables;    //!< All NULL when there is none.
	SG_VEC(sg_plan_patch)     patches;      //!< In precedence order.
	char                      hash[17];     //!< FNV-1a 64 of every script's name and text, in order, as hex.
	sg_words                  custom_types; //!< Every logical type from `types` some column uses, sorted.
	const sg_queries         *queries;      //!< The query files; borrowed.
} sg_plan;

/**
 * @brief Build the plan.
 * @return false, with errors reported, when the configuration asks for something the schema cannot
 *         give: writes on a view or virtual table, links on a table that is no junction, or two
 *         statements of one table that would get the same name.
 */
bool sg_plan_build(const sg_model *model, const sg_patchset *patches, const sg_config *config,
		const sg_queries *queries, sg_plan *out);

/**
 * @brief A column's logical type: its affinity's name, overridden by a matching `types.by_name`
 *        pattern, overridden by a matching `types.by_declared` entry.
 * @param custom Receives whether the type came from `types`.
 */
const char *sg_plan_logical_type(const sg_config *config, const sg_column *column, bool *custom);

/**
 * @brief The table whose typed id a column holds: its own table when it is the rowid alias, the
 *        parent when it is a one-column foreign key to the parent's rowid alias; NULL otherwise.
 */
const char *sg_plan_id_of(const sg_model *model, const sg_table *table, const sg_column *column);

/** @brief Write the plan as JSON. */
void sg_plan_write_json(const sg_plan *plan, sg_buf *out);

/** @brief The statement kind's name, as the JSON and the templates spell it. */
const char *sg_stmt_kind_name(sg_stmt_kind kind);

/** @brief Release everything. */
void sg_plan_free(sg_plan *plan);

#endif /* SQLGEN_PLAN_H */
