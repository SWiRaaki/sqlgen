<<<
output: \(prefix)_\(q.words | snake).h
each:   q in queries
>>>
\include("defs")
\call header(q.path, null)

#ifndef \(prefix | upper)_\(q.words | upper)_H
#define \(prefix | upper)_\(q.words | upper)_H

#include "\(prefix).h"
\if (not q.row_table and q.rows != "none") {

/* One row of \(q.name). */
typedef struct {
\align {
\for (c in q.columns) {
	\(c | ctype)\|\(c.words | cname);
\if (c.nullable and c | storage != "value") {
	bool\|has_\(c.words | snake);
\}
\}
\}
} \(prefix)_\(q.words | snake)_row;

/* Frees what the row owns and zeroes it. */
void \(prefix)_\(q.words | snake)_row_free(\(prefix)_\(q.words | snake)_row *row);

/* Fills out from the current row of a statement selecting what \(q.name) selects. */
int \(prefix)_\(q.words | snake)_read(sqlite3_stmt *stmt, \(prefix)_\(q.words | snake)_row *out);

/* Steps a cursor of \(q.name) to its next row: SQLITE_ROW with out filled, or SQLITE_DONE. */
int \(prefix)_\(q.words | snake)_next(\(prefix)_cursor *cursor, \(prefix)_\(q.words | snake)_row *out);
\}

/*
\for (line in q.doc) {
\if (line == "") {
 *
\} \else {
 * \(line)
\}
\}
\if (q.doc) {
 *
\}
\if (q.rows == "many") {
 * Opens a cursor over its rows; step it with \(prefix)_\(q.row_words | snake)_next.
\} \elif (q.rows == "exactly_one") {
 * Fills out with its one row: SQLITE_ROW.
\} \elif (q.rows == "one") {
 * Fills out with its row: SQLITE_ROW, or SQLITE_DONE when there is none.
\} \elif (q.writes) {
 * SQLITE_OK when it changed a row, SQLITE_DONE when it changed none.
\} \else {
 * SQLITE_OK when it ran.
\}
 */
\call signature(q);

#endif
