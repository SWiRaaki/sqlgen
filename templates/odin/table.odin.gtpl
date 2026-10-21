<<<
output: \(t.words | snake).odin
each:   t in tables
>>>
\include("defs")
\call header(t.origin.script, t.origin.patch)
package \(prefix)

import "base:runtime"

/* One row of \(t.name). */
\(t.words | ada)_Row :: struct {
\call members(t.columns)
}

/* Frees what the row owns and zeroes it. */
\(t.words | snake)_row_destroy :: proc(row: ^\(t.words | ada)_Row, allocator := context.allocator) {
\call destroy_members(t.columns)
	row^ = {}
}

@(private = "file")
COLUMNS := [?]cstring{
\for (c in t.columns) {
	\(c.name | odinstring),
\}
}

/* Reads the current row of a statement selecting exactly \(t.select_list). */
\(t.words | snake)_read :: proc(stmt: ^Sqlite3_Stmt, allocator: runtime.Allocator) -> (row: \(t.words | ada)_Row, rc: Result) {
	if rc = check_columns(stmt, COLUMNS[:]); rc != .OK {
		return
	}

\for (c in t.columns) {
\call read(c, loop.index, t.words | snake)
\}
	return row, .OK
}
\for (s in t.statements) {

\call otext(loop.index, s.sql)

/* \(s.sql) */
\if (s.kind == "get") {
\(t.words | snake)_\(s.words | snake) :: proc(
	ctx: ^Context,
\for (p in s.params) {
	\call param(t.columns | at(p.index), p.optional),
\}
	allocator := context.allocator,
) -> (
	row: \(t.words | ada)_Row,
	rc: Result,
) {
\} \elif (s.kind == "list") {
\(t.words | snake)_\(s.words | snake) :: proc(
	ctx: ^Context,
\for (p in s.params) {
	\call param(t.columns | at(p.index), p.optional),
\}
	allocator := context.allocator,
) -> (
	cursor: Cursor(\(t.words | ada)_Row),
	rc: Result,
) {
\} \elif (s.kind == "insert" and not t.without_rowid) {
\(t.words | snake)_\(s.words | snake) :: proc(
	ctx: ^Context,
\for (p in s.params) {
	\call param(t.columns | at(p.index), p.optional),
\}
) -> (
	key: \if (t.id_column) {\(t | idtype)\} \else {i64\},
	rc: Result,
) {
\} \else {
\(t.words | snake)_\(s.words | snake) :: proc(
	ctx: ^Context,
\for (p in s.params) {
	\call param(t.columns | at(p.index), p.optional),
\}
) -> (
	rc: Result,
) {
\}
\if (s.kind == "list") {
	if cursor, rc = cursor_open(ctx, .Stmt_\(t.words | ada)_\(s.words | ada), SQL_\(loop.index), \(t.words | snake)_read, allocator); rc != .OK {
		return
	}
\if (s.params) {

	stmt := cursor.stmt

\for (p in s.params) {
\call bind(t.columns | at(p.index), p.optional, loop.number, "cursor")
\}
\}
	return cursor, .OK
\} \else {
	stmt: ^Sqlite3_Stmt

	if stmt, rc = prepared(ctx, .Stmt_\(t.words | ada)_\(s.words | ada), SQL_\(loop.index)); rc != .OK {
		return
	}

\for (p in s.params) {
\call bind(t.columns | at(p.index), p.optional, loop.number, "statement")
\}
\if (s.kind == "get") {
	rc = Result(sqlite3_step(stmt))

	if rc == .ROW {
		row, rc = \(t.words | snake)_read(stmt, allocator)
		rc = .ROW if rc == .OK else rc
	}

	return row, finish(stmt, rc)
\} \elif (s.kind == "insert") {
	rc = run(stmt)
\if (not t.without_rowid) {

	if rc == .DONE {
\if (t.id_column) {
		raw: u64
		raw, rc = id_of(sqlite3_last_insert_rowid(ctx.db))
		key = \(t | idtype)(raw)
		rc = .DONE if rc == .OK else rc
\} \else {
		key = sqlite3_last_insert_rowid(ctx.db)
\}
	}
\}

	return \if (not t.without_rowid) {key, \}finish(stmt, .OK if rc == .DONE else rc)
\} \else {
	rc = run(stmt)

	if rc == .DONE {
		rc = .OK if sqlite3_changes(ctx.db) > 0 else .DONE
	}

	return finish(stmt, rc)
\}
\}
}
\}
