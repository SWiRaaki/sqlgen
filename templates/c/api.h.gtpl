<<<
output: \(prefix).h
>>>
\include("defs")
\call header("every script", null)

#ifndef \(prefix | upper)_H
#define \(prefix | upper)_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

/*
 * Every function returns an SQLite result code:
 *   SQLITE_ROW       a get found its row; a cursor stepped to its next row
 *   SQLITE_DONE      a get found no row; a cursor has no more rows; a write changed no row
 *   SQLITE_OK        a write changed at least one row; anything else succeeded
 *   SQLITE_MISMATCH  a statement handed to a _read function does not select that table's columns
 *   SQLITE_RANGE     an id outside what both uint64_t and SQLite's int64 can hold
 *   anything else    the error SQLite reported
 *
 * A row owns its text and blobs; every function filling a row frees what it held first, so a row
 * starts zeroed ({0}) and can then be reused. _row_free releases it.
 */

/* Text a row owns: allocated with malloc, NUL-terminated; len does not count the NUL. */
typedef struct {
	char   *ptr;
	size_t  len;
} \(prefix)_text;

/* Bytes a row owns: allocated with malloc; ptr is NULL when len is 0. */
typedef struct {
	unsigned char *ptr;
	size_t         len;
} \(prefix)_blob;

/* A column without a fixed type: type is SQLITE_INTEGER, SQLITE_FLOAT, SQLITE_TEXT, SQLITE_BLOB or
 * SQLITE_NULL, and says which member holds the value. */
typedef struct {
	int         type;
	int64_t     integer;
	double      real;
	\(prefix)_text text;
	\(prefix)_blob blob;
} \(prefix)_value;

/* One connection's prepared statements. */
typedef struct \(prefix)_ctx \(prefix)_ctx;

/* The rows of a list; step with the table's _next, release with \(prefix)_cursor_close. */
typedef struct {
	sqlite3_stmt *stmt;
	bool          owned;
} \(prefix)_cursor;

int      \(prefix)_ctx_open(\(prefix)_ctx **out, sqlite3 *db);
void     \(prefix)_ctx_close(\(prefix)_ctx *ctx);
sqlite3 *\(prefix)_ctx_db(const \(prefix)_ctx *ctx);

void \(prefix)_cursor_close(\(prefix)_cursor *cursor);
void \(prefix)_text_free(\(prefix)_text *text);
void \(prefix)_blob_free(\(prefix)_blob *blob);
void \(prefix)_value_free(\(prefix)_value *value);
\for (t in tables) {
\if (t.id_column) {

/* The primary key of \(t.name): a type of its own, so an id of another table does not fit. */
typedef struct {
	uint64_t v;
} \(t | idtype);

#define \(t | idtype | upper)(n) ((\(t | idtype)){ .v = (n) })
\}
\}
\if (embed_scripts) {

/* One schema script, its text exactly as it runs. */
typedef struct {
	const char *name;
	const char *sql;
} \(prefix)_script;

/* One patch: its version, parsed, and its scripts in the order they run. */
typedef struct {
	const char            *version;
	uint64_t               major;
	uint64_t               minor;
	uint64_t               patch;
	const char            *prerelease;
	const char            *build;
	const \(prefix)_script *scripts;
	size_t                 count;
} \(prefix)_patch;

/* The script creating the system tables, run before every patch; NULL when there is none. */
extern const \(prefix)_script *const \(prefix)_systables;

/* Every patch, in the order of their versions' precedence. */
extern const \(prefix)_patch *const \(prefix)_patches;
extern const size_t \(prefix)_patch_count;

/* A hash of every script's name and text, in order: it changes whenever the schema does. */
extern const char \(prefix)_schema_hash[];

/*
 * Brings db up to this program's schema, in one transaction: creates _sys_patch and _sys_script if
 * they are missing, runs the system-tables script, then applies every patch the database lacks, in
 * order, recording each with applied_by and the text of every script.
 *
 * Refused, with nothing changed: a patch the database has but this program does not carry (a newer
 * program migrated it); an applied script whose text changed, or that vanished or was added; a
 * missing patch older than one already applied.
 *
 * *applied receives how many patches ran. On failure *error receives a message, to release with
 * sqlite3_free. Either may be NULL.
 */
int \(prefix)_migrate(sqlite3 *db, const char *applied_by, size_t *applied, char **error);
\}

\for (t in tables) {
#include "\(prefix)_\(t.words | snake).h"
\}

#endif
