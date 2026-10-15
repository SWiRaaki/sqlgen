<<<
output: \(prefix)_\(q.words | snake).c
each:   q in queries
>>>
\include("defs")
\call header(q.path, null)

#include "\(prefix)_internal.h"
\if (not q.row_table and q.rows != "none") {

#include <string.h>

static const char *const column_names[] = {
\for (c in q.columns) {
	\(c.name | cstring),
\}
};

void \(prefix)_\(q.words | snake)_row_free(\(prefix)_\(q.words | snake)_row *row) {
\for (c in q.columns) {
\if (c | storage == "text" or c | storage == "blob" or c | storage == "value") {
	\(prefix)_\(c | storage)_free(&row->\(c.words | cname));
\}
\}
	memset(row, 0, sizeof *row);
}

int \(prefix)_\(q.words | snake)_read(sqlite3_stmt *stmt, \(prefix)_\(q.words | snake)_row *out) {
	int rc = \(prefix)__columns(stmt, column_names, \(q.columns | count));

	if (rc != SQLITE_OK) {
		return rc;
	}

	\(prefix)_\(q.words | snake)_row_free(out);

\for (c in q.columns) {
\call read(c, loop.index, q.words)
\}
	return SQLITE_OK;
}

int \(prefix)_\(q.words | snake)_next(\(prefix)_cursor *cursor, \(prefix)_\(q.words | snake)_row *out) {
	int rc = sqlite3_step(cursor->stmt);

	if (rc != SQLITE_ROW) {
		return rc;
	}

	rc = \(prefix)_\(q.words | snake)_read(cursor->stmt, out);
	return rc == SQLITE_OK ? SQLITE_ROW : rc;
}
\}

\call ctext("sql", "query", null, q.sql)

\call signature(q) {
\if (q.rows == "many") {
	int rc = \(prefix)__cursor(ctx, \(prefix | upper)_QUERY_\(q.words | upper), sql_query, out);

	if (rc != SQLITE_OK) {
		return rc;
	}

\if (q.params) {
	sqlite3_stmt *stmt = out->stmt;

\for (p in q.params) {
\call bind(p, false, p.index, "cursor")
\}
\}
	return SQLITE_OK;
\} \else {
	sqlite3_stmt *stmt;
	int           rc = \(prefix)__statement(ctx, \(prefix | upper)_QUERY_\(q.words | upper), sql_query, &stmt);

	if (rc != SQLITE_OK) {
		return rc;
	}

\for (p in q.params) {
\call bind(p, false, p.index, "statement")
\}
\if (q.rows != "none") {
	rc = sqlite3_step(stmt);

	if (rc == SQLITE_ROW) {
		rc = \(prefix)_\(q.row_words | snake)_read(stmt, out);
		rc = rc == SQLITE_OK ? SQLITE_ROW : rc;
	}
\} \elif (q.writes) {
	rc = \(prefix)__run(stmt);

	if (rc == SQLITE_DONE) {
		rc = sqlite3_changes(\(prefix)_ctx_db(ctx)) > 0 ? SQLITE_OK : SQLITE_DONE;
	}
\} \else {
	rc = \(prefix)__run(stmt);
	rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
\}

	return \(prefix)__finish(stmt, rc);
\}
}
