#ifndef SQLGEN_DIFF_H
#define SQLGEN_DIFF_H

/**
 * @file diff.h
 * @brief What changed between two schemas, and which statement changed it.
 *
 * Two models are compared table by table: a table's flags, its columns (declared type, NOT NULL,
 * default, collation, generated, primary key), their order, its unique keys, foreign keys,
 * indexes and triggers. A view is compared by its columns and its statement; so are a trigger, a
 * virtual table, and an index with a WHERE clause or an expression, whose parts the model does not
 * take apart. Statements are compared with their whitespace and letter case evened out.
 *
 * Given the snapshots in between as well -- one per statement that changed the schema -- tables
 * are followed through them, so that:
 *   - a table renamed (`alter table … rename to`) is reported as renamed, not dropped and created;
 *     the same for a column (`alter table … rename column`);
 *   - a table rebuilt the way SQLite's documentation describes (create the new table, copy, drop
 *     the old one, rename the new one) is compared with the table it replaced, and its changes
 *     are attributed to the `create table` of the new one;
 *   - every change names the statement that made it.
 *
 * A step counts as a rename when exactly one table disappears and one appears with the same
 * columns (or, within a table, one column disappears and one appears at its position with the
 * same declaration). Without steps -- a live database against the scripts -- names alone match.
 */

#include "model.h"
#include "util.h"

typedef enum {
	SG_CHANGE_ADD,
	SG_CHANGE_REMOVE,
	SG_CHANGE_ALTER,
	SG_CHANGE_RENAME
} sg_change_kind;

typedef enum {
	SG_THING_TABLE,       //!< A table, view or virtual table: see the tables' kind.
	SG_THING_COLUMN,
	SG_THING_ORDER,       //!< The order of a table's columns.
	SG_THING_UNIQUE,      //!< A unique key or the primary key.
	SG_THING_FOREIGN_KEY,
	SG_THING_INDEX,
	SG_THING_TRIGGER
} sg_thing;

typedef struct {
	sg_change_kind   kind;
	sg_thing         thing;
	char            *table;       //!< The table's name: its new one, or its last one when removed.
	char            *name;        //!< A column's, index's or trigger's name; NULL otherwise.
	char            *old_name;    //!< Renames: what it was called; for a column, the column's.
	char            *before;      //!< What it was, as text; NULL when added.
	char            *after;       //!< What it is, as text; NULL when removed.
	const sg_table  *old_table;   //!< In the older model; NULL when added.
	const sg_table  *new_table;   //!< In the newer model; NULL when removed.
	const sg_column *old_column;  //!< Columns only.
	const sg_column *new_column;
	bool             located;     //!< Whether @ref at names the statement.
	sg_origin        at;
} sg_change;

typedef SG_VEC(sg_change) sg_changes;

/**
 * @brief Compare @p steps[0] with @p steps[count - 1], following tables through the steps between.
 * @param at       The statement after which each step was taken; NULL when the steps are not
 *                 statements (two unrelated models), which also turns rename detection off.
 */
void sg_diff(const sg_model *const *steps, const sg_origin *at, size_t count, sg_changes *out);

/** @brief One line per change: `+` added, `-` removed, `~` changed, `>` renamed. */
void sg_diff_write_text(const sg_changes *changes, sg_buf *out);

/** @brief The changes as a JSON array, one object each. */
void sg_diff_write_json(const sg_changes *changes, sg_buf *out);

void sg_changes_free(sg_changes *changes);

#endif /* SQLGEN_DIFF_H */
