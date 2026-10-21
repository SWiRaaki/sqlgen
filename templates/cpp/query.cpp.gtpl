<<<
output: \(prefix)_\(q.words | snake).cpp
each:   q in queries
>>>
\include("defs")
\call header(q.path, null)

#include "\(prefix)_internal.hpp"

namespace \(prefix) {
namespace \(q.words | cppname) {
\if (not q.row_table and q.rows != "none") {

static const char *const column_names[] = {
\for (c in q.columns) {
	\(c.name | cstring),
\}
};

int read(sqlite3_stmt *stmt, Row &out) {
	int rc = detail::columns(stmt, column_names, \(q.columns | count));

	if (rc != SQLITE_OK) {
		return rc;
	}

	out = Row();

\for (c in q.columns) {
\call read(c, loop.index)
\}
	return SQLITE_OK;
}
\}

\call ctext("sql", "query", null, q.sql)

\call signature(q) {
\if (q.rows == "many") {
	int rc = detail::Access::cursor(ctx, detail::QUERY_\(q.words | upper), sql_query, out);

	if (rc != SQLITE_OK) {
		return rc;
	}

\if (q.params) {
	sqlite3_stmt *stmt = out.statement();

\for (p in q.params) {
\call bind(p, false, p.index, "cursor")
\}
\}
	return SQLITE_OK;
\} \else {
	sqlite3_stmt *stmt = nullptr;
	int           rc   = detail::Access::statement(ctx, detail::QUERY_\(q.words | upper), sql_query, stmt);

	if (rc != SQLITE_OK) {
		return rc;
	}

\for (p in q.params) {
\call bind(p, false, p.index, "statement")
\}
\if (q.rows != "none") {
	rc = sqlite3_step(stmt);

	if (rc == SQLITE_ROW) {
		rc = read(stmt, out);
		rc = rc == SQLITE_OK ? SQLITE_ROW : rc;
	}
\} \elif (q.writes) {
	rc = detail::run(stmt);

	if (rc == SQLITE_DONE) {
		rc = sqlite3_changes(ctx.db()) > 0 ? SQLITE_OK : SQLITE_DONE;
	}
\} \else {
	rc = detail::run(stmt);
	rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
\}

	return detail::finish(stmt, rc);
\}
}

} // namespace \(q.words | cppname)
} // namespace \(prefix)
