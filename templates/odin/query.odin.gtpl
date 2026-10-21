<<<
output: \(q.words | snake).odin
each:   q in queries
>>>
\include("defs")
\call header(q.path, null)
package \(prefix)
\if (not q.row_table and q.rows != "none") {

import "base:runtime"

/* One row of \(q.name). */
\(q.words | ada)_Row :: struct {
\call members(q.columns)
}

/* Frees what the row owns and zeroes it. */
\(q.words | snake)_row_destroy :: proc(row: ^\(q.words | ada)_Row, allocator := context.allocator) {
\call destroy_members(q.columns)
	row^ = {}
}

@(private = "file")
COLUMNS := [?]cstring{
\for (c in q.columns) {
	\(c.name | odinstring),
\}
}

/* Reads the current row of a statement selecting what \(q.name) selects. */
\(q.words | snake)_read :: proc(stmt: ^Sqlite3_Stmt, allocator: runtime.Allocator) -> (row: \(q.words | ada)_Row, rc: Result) {
	if rc = check_columns(stmt, COLUMNS[:]); rc != .OK {
		return
	}

\for (c in q.columns) {
\call read(c, loop.index, q.words | snake)
\}
	return row, .OK
}
\}

\call otext("query", q.sql)

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
 * A cursor over its rows.
\} \elif (q.rows == "exactly_one") {
 * Its one row: .ROW.
\} \elif (q.rows == "one") {
 * Its row: .ROW, or .DONE when there is none.
\} \elif (q.writes) {
 * .OK when it changed a row, .DONE when it changed none.
\} \else {
 * .OK when it ran.
\}
 */
\(q.words | odinname) :: proc(
	ctx: ^Context,
\for (p in q.params) {
	\call param(p, false),
\}
\if (q.rows != "none") {
	allocator := context.allocator,
\}
) -> (
\if (q.rows == "many") {
	cursor: Cursor(\(q.row_words | ada)_Row),
\} \elif (q.rows != "none") {
	row: \(q.row_words | ada)_Row,
\}
	rc: Result,
) {
\if (q.rows == "many") {
	if cursor, rc = cursor_open(ctx, .Query_\(q.words | ada), SQL_query, \(q.row_words | snake)_read, allocator); rc != .OK {
		return
	}
\if (q.params) {

	stmt := cursor.stmt

\for (p in q.params) {
\call bind(p, false, p.index, "cursor")
\}
\}
	return cursor, .OK
\} \else {
	stmt: ^Sqlite3_Stmt

	if stmt, rc = prepared(ctx, .Query_\(q.words | ada), SQL_query); rc != .OK {
		return
	}

\for (p in q.params) {
\call bind(p, false, p.index, "statement")
\}
\if (q.rows != "none") {
	rc = Result(sqlite3_step(stmt))

	if rc == .ROW {
		row, rc = \(q.row_words | snake)_read(stmt, allocator)
		rc = .ROW if rc == .OK else rc
	}

	return row, finish(stmt, rc)
\} \elif (q.writes) {
	rc = run(stmt)

	if rc == .DONE {
		rc = .OK if sqlite3_changes(ctx.db) > 0 else .DONE
	}

	return finish(stmt, rc)
\} \else {
	rc = run(stmt)
	return finish(stmt, .OK if rc == .DONE else rc)
\}
\}
}
