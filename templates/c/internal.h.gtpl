<<<
output: \(prefix)_internal.h
>>>
\include("defs")
\call header("every script", null)

#ifndef \(prefix | upper)_INTERNAL_H
#define \(prefix | upper)_INTERNAL_H

/* What the generated sources share; not part of the interface. */

#include "\(prefix).h"

enum {
\for (t in tables) {
\for (s in t.statements) {
	\(prefix | upper)_STMT_\(t.words | upper)_\(s.words | upper),
\}
\}
	\(prefix | upper)_STATEMENT_COUNT
};

int \(prefix)__statement(\(prefix)_ctx *ctx, int slot, const char *sql, sqlite3_stmt **out);
int \(prefix)__cursor(\(prefix)_ctx *ctx, int slot, const char *sql, \(prefix)_cursor *out);
int \(prefix)__cursor_fail(\(prefix)_cursor *cursor, int rc);
int \(prefix)__finish(sqlite3_stmt *stmt, int rc);
int \(prefix)__run(sqlite3_stmt *stmt);
int \(prefix)__columns(sqlite3_stmt *stmt, const char *const *names, int count);
int \(prefix)__id(sqlite3_int64 value, uint64_t *out);

int \(prefix)__read_text(sqlite3_stmt *stmt, int column, \(prefix)_text *out);
int \(prefix)__read_blob(sqlite3_stmt *stmt, int column, \(prefix)_blob *out);
int \(prefix)__read_value(sqlite3_stmt *stmt, int column, \(prefix)_value *out);
int \(prefix)__read_id(sqlite3_stmt *stmt, int column, uint64_t *out);

int \(prefix)__bind_text(sqlite3_stmt *stmt, int index, const char *text, size_t len);
int \(prefix)__bind_blob(sqlite3_stmt *stmt, int index, const void *bytes, size_t len);
int \(prefix)__bind_value(sqlite3_stmt *stmt, int index, const \(prefix)_value *value);
int \(prefix)__bind_id(sqlite3_stmt *stmt, int index, uint64_t id);

#endif
