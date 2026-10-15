<<<
output: \(prefix)_\(t.words | snake).h
each:   t in tables
>>>
\include("defs")
\call header(t.origin.script, t.origin.patch)

#ifndef \(prefix | upper)_\(t.words | upper)_H
#define \(prefix | upper)_\(t.words | upper)_H

#include "\(prefix).h"

/* One row of \(t.name). */
typedef struct {
\align {
\for (c in t.columns) {
	\(c | ctype)\|\(c.words | cname);
\if (c.nullable and c | storage != "value") {
	bool\|has_\(c.words | snake);
\}
\}
\}
} \(prefix)_\(t.words | snake)_row;

/* Frees what the row owns and zeroes it. */
void \(prefix)_\(t.words | snake)_row_free(\(prefix)_\(t.words | snake)_row *row);

/* Fills out from the current row of a statement selecting exactly \(t.select_list). */
int \(prefix)_\(t.words | snake)_read(sqlite3_stmt *stmt, \(prefix)_\(t.words | snake)_row *out);

/* Steps a cursor of \(t.name) to its next row: SQLITE_ROW with out filled, or SQLITE_DONE. */
int \(prefix)_\(t.words | snake)_next(\(prefix)_cursor *cursor, \(prefix)_\(t.words | snake)_row *out);
\for (s in t.statements) {

/* \(s.sql) */
\if (s.kind == "get") {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\(prefix)_\(t.words | snake)_row *out);
\} \elif (s.kind == "list") {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\(prefix)_cursor *out);
\} \elif (s.kind == "insert" and not t.without_rowid) {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\if (t.id_column) {\(t | idtype)\} \else {int64_t\} *out_key);
\} \else {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\});
\}
\}

#endif
