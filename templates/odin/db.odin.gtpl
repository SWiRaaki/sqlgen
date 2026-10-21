<<<
output: \(prefix).odin
>>>
\include("defs")
\call header("every script", null)
package \(prefix)

import "base:runtime"

/*
 * Every procedure reports an SQLite result code:
 *   .ROW       a get or a one-row query found its row; a cursor stepped to its next row
 *   .DONE      a get or a one-row query found no row; a cursor has no more rows; a write changed
 *              no row
 *   .OK        a write changed at least one row; anything else succeeded
 *   .MISMATCH  a statement handed to a read procedure does not select that row's columns
 *   .RANGE     an id outside what both u64 and SQLite's int64 can hold
 *   .NOMEM     the allocator ran out
 *   anything else, the error SQLite reported
 *
 * A row's text and blobs are allocated with the allocator the reading procedure was given, and
 * <row>_destroy frees them; with an arena, nothing needs freeing.
 */
Result :: enum i32 {
	OK         = 0,
	ERROR      = 1,
	INTERNAL   = 2,
	PERM       = 3,
	ABORT      = 4,
	BUSY       = 5,
	LOCKED     = 6,
	NOMEM      = 7,
	READONLY   = 8,
	INTERRUPT  = 9,
	IOERR      = 10,
	CORRUPT    = 11,
	NOTFOUND   = 12,
	FULL       = 13,
	CANTOPEN   = 14,
	PROTOCOL   = 15,
	EMPTY      = 16,
	SCHEMA     = 17,
	TOOBIG     = 18,
	CONSTRAINT = 19,
	MISMATCH   = 20,
	MISUSE     = 21,
	NOLFS      = 22,
	AUTH       = 23,
	FORMAT     = 24,
	RANGE      = 25,
	NOTADB     = 26,
	NOTICE     = 27,
	WARNING    = 28,
	ROW        = 100,
	DONE       = 101,
}

/* A column without a fixed type; nil is NULL. */
Value :: union {
	i64,
	f64,
	string,
	[]u8,
}

/* Frees what a value owns. */
value_destroy :: proc(value: Value, allocator := context.allocator) {
	#partial switch it in value {
	case string:
		delete(it, allocator)
	case []u8:
		delete(it, allocator)
	}
}
\for (t in tables) {
\if (t.id_column) {

/* The primary key of \(t.name): a type of its own, so an id of another table does not fit. */
\(t | idtype) :: distinct u64
\}
\}

/* Every statement a connection prepares once and keeps. */
Statement :: enum {
\for (t in tables) {
\for (s in t.statements) {
	Stmt_\(t.words | ada)_\(s.words | ada),
\}
\}
\for (q in queries) {
	Query_\(q.words | ada),
\}
}

/* One connection's prepared statements. It does not own the connection. */
Context :: struct {
	db:         ^Sqlite3,
	statements: [Statement]^Sqlite3_Stmt,
}

context_make :: proc(db: ^Sqlite3) -> Context {
	return Context{db = db}
}

/* Finalizes every statement; the connection stays open. */
context_destroy :: proc(ctx: ^Context) {
	for stmt in ctx.statements {
		sqlite3_finalize(stmt)
	}

	ctx^ = {}
}

/* The rows of a list or a query, each read into a Row. */
Cursor :: struct($Row: typeid) {
	stmt:      ^Sqlite3_Stmt,
	owned:     bool,
	read:      proc(stmt: ^Sqlite3_Stmt, allocator: runtime.Allocator) -> (Row, Result),
	allocator: runtime.Allocator,
}

/* .ROW with the next row, .DONE after the last, or an error. */
cursor_next :: proc(cursor: ^Cursor($Row)) -> (row: Row, rc: Result) {
	if cursor.stmt == nil || cursor.read == nil {
		return {}, .MISUSE
	}

	rc = Result(sqlite3_step(cursor.stmt))

	if rc != .ROW {
		return {}, rc
	}

	row, rc = cursor.read(cursor.stmt, cursor.allocator)
	return row, .ROW if rc == .OK else rc
}

/* Ends a cursor; every cursor is closed once its rows are no longer wanted. */
cursor_close :: proc(cursor: ^Cursor($Row)) {
	if cursor.stmt == nil {
		return
	}

	if cursor.owned {
		sqlite3_finalize(cursor.stmt)
	} else {
		sqlite3_reset(cursor.stmt)
		sqlite3_clear_bindings(cursor.stmt)
	}

	cursor.stmt = nil
}

/* A statement is prepared once per connection and reused; each use starts it afresh. */
@(private = "package")
prepared :: proc(ctx: ^Context, slot: Statement, sql: string) -> (^Sqlite3_Stmt, Result) {
	if ctx.statements[slot] == nil {
		stmt: ^Sqlite3_Stmt
		rc := Result(sqlite3_prepare_v3(ctx.db, raw_data(sql), i32(len(sql)), SQLITE_PREPARE_PERSISTENT, &stmt, nil))

		if rc != .OK {
			return nil, rc
		}

		ctx.statements[slot] = stmt
	} else {
		sqlite3_reset(ctx.statements[slot])
		sqlite3_clear_bindings(ctx.statements[slot])
	}

	return ctx.statements[slot], .OK
}

/* A list whose statement is still stepping for another cursor gets a statement of its own. */
@(private = "package")
cursor_open :: proc(
	ctx: ^Context,
	slot: Statement,
	sql: string,
	read: proc(stmt: ^Sqlite3_Stmt, allocator: runtime.Allocator) -> ($Row, Result),
	allocator: runtime.Allocator,
) -> (
	cursor: Cursor(Row),
	rc: Result,
) {
	busy := ctx.statements[slot] != nil && sqlite3_stmt_busy(ctx.statements[slot]) != 0
	stmt: ^Sqlite3_Stmt

	if busy {
		rc = Result(sqlite3_prepare_v3(ctx.db, raw_data(sql), i32(len(sql)), 0, &stmt, nil))
	} else {
		stmt, rc = prepared(ctx, slot, sql)
	}

	if rc == .OK {
		cursor = Cursor(Row){stmt = stmt, owned = busy, read = read, allocator = allocator}
	}

	return
}

@(private = "package")
finish :: proc(stmt: ^Sqlite3_Stmt, rc: Result) -> Result {
	sqlite3_reset(stmt)
	sqlite3_clear_bindings(stmt)
	return rc
}

/* Steps to the end, past any rows a RETURNING clause yields. */
@(private = "package")
run :: proc(stmt: ^Sqlite3_Stmt) -> Result {
	rc := Result(sqlite3_step(stmt))

	for rc == .ROW {
		rc = Result(sqlite3_step(stmt))
	}

	return rc
}

/* The statement must select exactly these columns, in this order. */
@(private = "package")
check_columns :: proc(stmt: ^Sqlite3_Stmt, names: []cstring) -> Result {
	if int(sqlite3_column_count(stmt)) != len(names) {
		return .MISMATCH
	}

	for name, i in names {
		have := sqlite3_column_name(stmt, i32(i))

		if have == nil || sqlite3_stricmp(have, name) != 0 {
			return .MISMATCH
		}
	}

	return .OK
}

@(private = "package")
id_of :: proc(value: i64) -> (u64, Result) {
	if value < 0 {
		return 0, .RANGE
	}

	return u64(value), .OK
}

@(private = "package")
read_id :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> (u64, Result) {
	return id_of(sqlite3_column_int64(stmt, column))
}

@(private = "package")
read_bytes :: proc(data: [^]u8, n: int, allocator: runtime.Allocator) -> ([]u8, Result) {
	out, err := make([]u8, n, allocator)

	if err != nil {
		return nil, .NOMEM
	}

	if n > 0 {
		copy(out, data[:n])
	}

	return out, .OK
}

@(private = "package")
read_text :: proc(stmt: ^Sqlite3_Stmt, column: i32, allocator: runtime.Allocator) -> (string, Result) {
	data := sqlite3_column_text(stmt, column)
	n := int(sqlite3_column_bytes(stmt, column))

	if data == nil && n > 0 {
		return "", .NOMEM
	}

	bytes, rc := read_bytes(data, n, allocator)
	return string(bytes), rc
}

@(private = "package")
read_blob :: proc(stmt: ^Sqlite3_Stmt, column: i32, allocator: runtime.Allocator) -> ([]u8, Result) {
	data := cast([^]u8)sqlite3_column_blob(stmt, column)
	n := int(sqlite3_column_bytes(stmt, column))

	if data == nil && n > 0 {
		return nil, .NOMEM
	}

	return read_bytes(data, n, allocator)
}

@(private = "package")
read_value :: proc(stmt: ^Sqlite3_Stmt, column: i32, allocator: runtime.Allocator) -> (Value, Result) {
	switch sqlite3_column_type(stmt, column) {
	case SQLITE_INTEGER:
		return sqlite3_column_int64(stmt, column), .OK
	case SQLITE_FLOAT:
		return sqlite3_column_double(stmt, column), .OK
	case SQLITE_TEXT:
		text, rc := read_text(stmt, column, allocator)
		return text, rc
	case SQLITE_BLOB:
		blob, rc := read_blob(stmt, column, allocator)
		return blob, rc
	}

	return nil, .OK
}

/* SQLite reads the caller's bytes (borrowed) or a copy of its own; an empty text or blob still
 * needs a pointer, or it would be NULL. */
@(private = "package")
EMPTY := [1]u8{}

@(private = "package")
bind_text :: proc(stmt: ^Sqlite3_Stmt, index: i32, text: string, copy: bool) -> Result {
	data := raw_data(text) if len(text) > 0 else raw_data(EMPTY[:])
	return Result(sqlite3_bind_text64(stmt, index, data, u64(len(text)), SQLITE_TRANSIENT if copy else SQLITE_STATIC, SQLITE_UTF8))
}

@(private = "package")
bind_blob :: proc(stmt: ^Sqlite3_Stmt, index: i32, bytes: []u8, copy: bool) -> Result {
	data := raw_data(bytes) if len(bytes) > 0 else raw_data(EMPTY[:])
	return Result(sqlite3_bind_blob64(stmt, index, data, u64(len(bytes)), SQLITE_TRANSIENT if copy else SQLITE_STATIC))
}

@(private = "package")
bind_value :: proc(stmt: ^Sqlite3_Stmt, index: i32, value: Value, copy: bool) -> Result {
	switch it in value {
	case i64:
		return Result(sqlite3_bind_int64(stmt, index, it))
	case f64:
		return Result(sqlite3_bind_double(stmt, index, it))
	case string:
		return bind_text(stmt, index, it, copy)
	case []u8:
		return bind_blob(stmt, index, it, copy)
	}

	return Result(sqlite3_bind_null(stmt, index))
}

@(private = "package")
bind_id :: proc(stmt: ^Sqlite3_Stmt, index: i32, id: u64) -> Result {
	if id > u64(max(i64)) {
		return .RANGE
	}

	return Result(sqlite3_bind_int64(stmt, index, i64(id)))
}
