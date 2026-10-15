#ifndef SQLGEN_BYTECODE_H
#define SQLGEN_BYTECODE_H

/**
 * @file bytecode.h
 * @brief What a statement's bytecode says about its parameters, its result and its rows.
 *
 * SQLite compiles every statement into a program for its virtual machine, which `explain` lists
 * one instruction a row. The program is walked here as a control-flow graph, and every register is
 * followed through it: which storage classes it can hold, whether it can be NULL, and where its
 * value came from -- a table column, a parameter, a constant. Values are followed into records and
 * out again, through sorters and temporary tables.
 *
 * From that:
 *   - a result column's storage classes and whether it can be NULL: a column read takes the
 *     column's type, NULL included when the column is nullable or its cursor can be a left join's
 *     null row (NullRow); a cast its target type; a built-in function or aggregate its result
 *     type; arithmetic integer or real; concatenation text; a comparison an integer;
 *   - a parameter's column and type: from the index key it seeks with (Affinity, Seek*), the
 *     column it is compared with (Eq, Lt, …), or the record position it is stored at (MakeRecord
 *     before Insert);
 *   - how many rows: none for a statement without ResultRow; at most one when no ResultRow lies on
 *     a cycle of the graph; exactly one when, besides, no path to Halt avoids it. A LIMIT 1
 *     counter (DecrJumpZero on a register holding 1) removes its loop's back edge.
 *
 * The program is compiled with foreign key enforcement off: its checks only look rows up, and
 * would make a parameter stored into a child column seem compared with the parent key as well.
 *
 * The analysis never guesses. An instruction it does not model has the registers it writes -- read
 * from the instruction's comment, which `SQLITE_ENABLE_EXPLAIN_COMMENTS` provides -- marked
 * unknown, and any jump target it carries kept as a possible edge. Unknown values surface as
 * SG_BT_UNKNOWN; extra edges only ever make a column more nullable or a statement return more rows.
 *
 * The bytecode belongs to the SQLite version sqlgen is built with; its tests pin what this file
 * reads from it.
 */

#include <sqlite3.h>

#include "model.h"

/** @brief Storage classes a value can have; a set of these bits. */
enum {
	SG_BT_NULL    = 1 << 0,
	SG_BT_INT     = 1 << 1,
	SG_BT_REAL    = 1 << 2,
	SG_BT_TEXT    = 1 << 3,
	SG_BT_BLOB    = 1 << 4,
	SG_BT_UNKNOWN = 1 << 5   //!< Anything; the analysis could not tell.
};

/** @brief Where a value came from: exactly one column of one table, or nowhere in particular. */
typedef struct {
	const sg_table *table;    //!< NULL when the value is no single column.
	int             column;   //!< Index into the table's columns; -1 for a rowid without an alias.
} sg_bsource;

typedef struct {
	unsigned   types;    //!< SG_BT_* bits; SG_BT_NULL set means it can be NULL.
	sg_bsource source;
} sg_bresult;

typedef struct {
	unsigned   types;    //!< What it is compared with or stored as, NULL left out; 0 when nothing says.
	sg_bsource source;   //!< The one column it is compared with or stored into, if there is one.
	bool       several;  //!< It meets more than one column, so no single one names it.
} sg_bparam;

typedef enum {
	SG_ROWS_NONE,         //!< No result rows: a write without RETURNING.
	SG_ROWS_AT_MOST_ONE,
	SG_ROWS_EXACTLY_ONE,
	SG_ROWS_MANY
} sg_rows;

typedef SG_VEC(sg_bresult) sg_bresults;
typedef SG_VEC(sg_bparam)  sg_bparams;

typedef struct {
	sg_bresults results;      //!< One per result column.
	sg_bparams  params;       //!< One per parameter, in sqlite3_bind_parameter_index order.
	sg_rows     rows;
} sg_banalysis;

/**
 * @brief Analyse @p sql, which must prepare against @p db, whose schema @p model describes.
 * @return false, with an error reported at @p file, when the program cannot be read.
 */
bool sg_bytecode_analyze(sqlite3 *db, const sg_model *model, const char *sql, const char *file,
		sg_banalysis *out);

void sg_banalysis_free(sg_banalysis *analysis);

/** @brief The storage classes of a column as its declared type gives them, NULL included when nullable. */
unsigned sg_bytecode_column_types(const sg_column *column);

#endif /* SQLGEN_BYTECODE_H */
