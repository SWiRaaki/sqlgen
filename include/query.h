#ifndef SQLGEN_QUERY_H
#define SQLGEN_QUERY_H

/**
 * @file query.h
 * @brief Query files: one statement each, typed by its bytecode, for the generator to wrap.
 *
 * Every `*.sql` file the `queries` patterns name holds exactly one statement and is named after the
 * file: `chapters_for_series.sql` becomes the query `chapters_for_series`. The statement is
 * prepared against the loaded schema, so a mistake in it is reported at its line and column.
 *
 * Its parameters, result columns and rows come from @ref sg_bytecode_analyze. A parameter or
 * column that is one table column takes that column's logical type and typed id. Whatever the
 * analysis cannot tell is an error, until a comment line says it:
 *
 *     -- @param since integer          a parameter's type; `null` after it allows NULL
 *     -- @column ratio real null       a result column's type, and that it can be NULL
 *     -- @returns one                  at most one row (or `many`)
 *     -- @allow scan                   a full table scan is intended
 *
 * Types in these lines are affinity names: integer, real, text, blob, or numeric for a value that
 * may be integer or real.
 *
 * A named parameter (`:name`, `@name`, `$name`) keeps its name. A positional one (`?`, `?3`) takes
 * the name of the one column it is compared with or stored into, and is an error without one.
 */

#include <sqlite3.h>

#include "bytecode.h"
#include "config.h"
#include "model.h"

/** @brief A parameter or a result column, typed. */
typedef struct {
	char           *name;
	sg_names        words;
	int             index;      //!< Parameters: the 1-based bind index.
	char           *type;       //!< Logical type.
	bool            custom;     //!< The type came from `types`.
	sg_affinity     affinity;   //!< How it is stored: what reads and binds it.
	bool            nullable;
	const char     *id_of;      //!< The table whose typed id it holds; NULL otherwise.
} sg_query_field;

typedef SG_VEC(sg_query_field) sg_query_fields;

typedef struct {
	char            *name;
	sg_names         words;
	char            *path;
	unsigned         line;        //!< Where the statement starts, for messages.
	unsigned         col;
	char            *sql;         //!< The statement as written, without its `;`.
	sg_names         doc;         //!< The comment lines before it that are no annotation, without `--`.
	sg_query_fields  params;
	sg_query_fields  columns;
	sg_rows          rows;
	bool             writes;
	const sg_table  *row_table;   //!< It selects exactly this table's columns, so its row type is that.
	bool             allow_scan;
	sg_names         scans;       //!< What `explain query plan` says it scans in full.
} sg_query;

typedef SG_VEC(sg_query) sg_queries;

/**
 * @brief Load every query file the configuration names, in file-name order.
 * @return false, with every problem reported, when any query cannot be used.
 */
bool sg_queries_load(sqlite3 *db, const sg_model *model, const sg_config *config, sg_queries *out);

void sg_queries_free(sg_queries *queries);

#endif /* SQLGEN_QUERY_H */
