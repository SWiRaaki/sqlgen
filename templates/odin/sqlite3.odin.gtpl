<<<
output: sqlite3.odin
>>>
\include("defs")
\call header("every script", null)
package \(prefix)

/*
 * The part of SQLite's C interface this package calls, linked as the target's `link` says. Public,
 * so a program without bindings of its own can open a database with them.
 */

foreign import sqlite \(target.link | odinstring)

Sqlite3 :: struct {}
Sqlite3_Stmt :: struct {}

SQLITE_INTEGER :: 1
SQLITE_FLOAT :: 2
SQLITE_TEXT :: 3
SQLITE_BLOB :: 4
SQLITE_NULL :: 5

SQLITE_UTF8 :: 1
SQLITE_PREPARE_PERSISTENT :: 0x01

SQLITE_OPEN_READWRITE :: 0x00000002
SQLITE_OPEN_CREATE :: 0x00000004

/* The destructor argument of the bind procedures: the caller keeps the bytes alive (static), or
 * SQLite copies them first (transient). */
SQLITE_STATIC :: uintptr(0)
SQLITE_TRANSIENT :: ~uintptr(0)

@(default_calling_convention = "c")
foreign sqlite {
	sqlite3_open_v2 :: proc(filename: cstring, db: ^^Sqlite3, flags: i32, vfs: cstring) -> i32 ---
	sqlite3_close_v2 :: proc(db: ^Sqlite3) -> i32 ---
	sqlite3_exec :: proc(db: ^Sqlite3, sql: cstring, callback: rawptr, arg: rawptr, errmsg: ^cstring) -> i32 ---
	sqlite3_errmsg :: proc(db: ^Sqlite3) -> cstring ---
	sqlite3_errstr :: proc(rc: i32) -> cstring ---
	sqlite3_errcode :: proc(db: ^Sqlite3) -> i32 ---
	sqlite3_free :: proc(ptr: rawptr) ---
	sqlite3_get_autocommit :: proc(db: ^Sqlite3) -> i32 ---
	sqlite3_changes :: proc(db: ^Sqlite3) -> i32 ---
	sqlite3_last_insert_rowid :: proc(db: ^Sqlite3) -> i64 ---
	sqlite3_stricmp :: proc(a: cstring, b: cstring) -> i32 ---

	sqlite3_prepare_v3 :: proc(db: ^Sqlite3, sql: [^]u8, n: i32, flags: u32, stmt: ^^Sqlite3_Stmt, tail: ^[^]u8) -> i32 ---
	sqlite3_step :: proc(stmt: ^Sqlite3_Stmt) -> i32 ---
	sqlite3_reset :: proc(stmt: ^Sqlite3_Stmt) -> i32 ---
	sqlite3_clear_bindings :: proc(stmt: ^Sqlite3_Stmt) -> i32 ---
	sqlite3_finalize :: proc(stmt: ^Sqlite3_Stmt) -> i32 ---
	sqlite3_stmt_busy :: proc(stmt: ^Sqlite3_Stmt) -> i32 ---

	sqlite3_bind_int64 :: proc(stmt: ^Sqlite3_Stmt, index: i32, value: i64) -> i32 ---
	sqlite3_bind_double :: proc(stmt: ^Sqlite3_Stmt, index: i32, value: f64) -> i32 ---
	sqlite3_bind_text64 :: proc(stmt: ^Sqlite3_Stmt, index: i32, text: [^]u8, n: u64, destructor: uintptr, encoding: u8) -> i32 ---
	sqlite3_bind_blob64 :: proc(stmt: ^Sqlite3_Stmt, index: i32, bytes: [^]u8, n: u64, destructor: uintptr) -> i32 ---
	sqlite3_bind_null :: proc(stmt: ^Sqlite3_Stmt, index: i32) -> i32 ---

	sqlite3_column_count :: proc(stmt: ^Sqlite3_Stmt) -> i32 ---
	sqlite3_column_name :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> cstring ---
	sqlite3_column_type :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> i32 ---
	sqlite3_column_int64 :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> i64 ---
	sqlite3_column_double :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> f64 ---
	sqlite3_column_text :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> [^]u8 ---
	sqlite3_column_blob :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> rawptr ---
	sqlite3_column_bytes :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> i32 ---
}
