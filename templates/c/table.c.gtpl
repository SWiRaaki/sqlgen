<<<
output: \(prefix)_\(t.words | snake).c
each:   t in tables
>>>
\include("defs")
\call header(t.origin.script, t.origin.patch)

#include "\(prefix)_internal.h"

#include <string.h>

static const char *const column_names[] = {
\for (c in t.columns) {
	\(c.name | cstring),
\}
};

void \(prefix)_\(t.words | snake)_row_free(\(prefix)_\(t.words | snake)_row *row) {
\for (c in t.columns) {
\if (c | storage == "text" or c | storage == "blob" or c | storage == "value") {
	\(prefix)_\(c | storage)_free(&row->\(c.words | cname));
\}
\}
	memset(row, 0, sizeof *row);
}

int \(prefix)_\(t.words | snake)_read(sqlite3_stmt *stmt, \(prefix)_\(t.words | snake)_row *out) {
	int rc = \(prefix)__columns(stmt, column_names, \(t.columns | count));

	if (rc != SQLITE_OK) {
		return rc;
	}

	\(prefix)_\(t.words | snake)_row_free(out);

\for (c in t.columns) {
\call read(c, loop.index, t.words)
\}
	return SQLITE_OK;
}

int \(prefix)_\(t.words | snake)_next(\(prefix)_cursor *cursor, \(prefix)_\(t.words | snake)_row *out) {
	int rc = sqlite3_step(cursor->stmt);

	if (rc != SQLITE_ROW) {
		return rc;
	}

	rc = \(prefix)_\(t.words | snake)_read(cursor->stmt, out);
	return rc == SQLITE_OK ? SQLITE_ROW : rc;
}
\for (s in t.statements) {

\call ctext("sql", loop.index, null, s.sql)

\if (s.kind == "list") {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\(prefix)_cursor *out) {
	int rc = \(prefix)__cursor(ctx, \(prefix | upper)_STMT_\(t.words | upper)_\(s.words | upper), sql_\(loop.index), out);

	if (rc != SQLITE_OK) {
		return rc;
	}

	sqlite3_stmt *stmt = out->stmt;

\for (p in s.params) {
\call bind(t.columns | at(p.index), p.optional, loop.number, "cursor")
\}
	return SQLITE_OK;
}
\} \else {
\if (s.kind == "get") {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\(prefix)_\(t.words | snake)_row *out) {
\} \elif (s.kind == "insert" and not t.without_rowid) {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\if (t.id_column) {\(t | idtype)\} \else {int64_t\} *out_key) {
\} \else {
int \(prefix)_\(t.words | snake)_\(s.words | snake)(\(prefix)_ctx *ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\}) {
\}
	sqlite3_stmt *stmt;
	int           rc = \(prefix)__statement(ctx, \(prefix | upper)_STMT_\(t.words | upper)_\(s.words | upper),
			sql_\(loop.index), &stmt);

	if (rc != SQLITE_OK) {
		return rc;
	}

\for (p in s.params) {
\call bind(t.columns | at(p.index), p.optional, loop.number, "statement")
\}
\if (s.kind == "get") {
	rc = sqlite3_step(stmt);

	if (rc == SQLITE_ROW) {
		rc = \(prefix)_\(t.words | snake)_read(stmt, out);
		rc = rc == SQLITE_OK ? SQLITE_ROW : rc;
	}

	return \(prefix)__finish(stmt, rc);
\} \elif (s.kind == "insert") {
	rc = \(prefix)__run(stmt);
\if (not t.without_rowid) {

	if (rc == SQLITE_DONE && out_key) {
\if (t.id_column) {
		rc = \(prefix)__id(sqlite3_last_insert_rowid(\(prefix)_ctx_db(ctx)), &out_key->v);
		rc = rc == SQLITE_OK ? SQLITE_DONE : rc;
\} \else {
		*out_key = sqlite3_last_insert_rowid(\(prefix)_ctx_db(ctx));
\}
	}
\}

	return \(prefix)__finish(stmt, rc == SQLITE_DONE ? SQLITE_OK : rc);
\} \else {
	rc = \(prefix)__run(stmt);

	if (rc == SQLITE_DONE) {
		rc = sqlite3_changes(\(prefix)_ctx_db(ctx)) > 0 ? SQLITE_OK : SQLITE_DONE;
	}

	return \(prefix)__finish(stmt, rc);
\}
}
\}
\}
