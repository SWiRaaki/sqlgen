<<<
output: \(prefix)_\(t.words | snake).cpp
each:   t in tables
>>>
\include("defs")
\call header(t.origin.script, t.origin.patch)

#include "\(prefix)_internal.hpp"

namespace \(prefix) {
namespace \(t.words | cppname) {

static const char *const column_names[] = {
\for (c in t.columns) {
	\(c.name | cstring),
\}
};

int read(sqlite3_stmt *stmt, Row &out) {
	int rc = detail::columns(stmt, column_names, \(t.columns | count));

	if (rc != SQLITE_OK) {
		return rc;
	}

	out = Row();

\for (c in t.columns) {
\call read(c, loop.index)
\}
	return SQLITE_OK;
}
\for (s in t.statements) {

\call ctext("sql", loop.index, null, s.sql)

\if (s.kind == "list") {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		Cursor<Row> &out) {
	int rc = detail::Access::cursor(ctx, detail::STMT_\(t.words | upper)_\(s.words | upper), sql_\(loop.index), out);

	if (rc != SQLITE_OK) {
		return rc;
	}

\if (s.params) {
	sqlite3_stmt *stmt = out.statement();

\for (p in s.params) {
\call bind(t.columns | at(p.index), p.optional, loop.number, "cursor")
\}
\}
	return SQLITE_OK;
}
\} \else {
\if (s.kind == "get") {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		Row &out) {
\} \elif (s.kind == "insert" and not t.without_rowid) {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\if (t.id_column) {\(t | idtype)\} \else {int64_t\} *out_key) {
\} \else {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\}) {
\}
	sqlite3_stmt *stmt = nullptr;
	int           rc   = detail::Access::statement(ctx, detail::STMT_\(t.words | upper)_\(s.words | upper), sql_\(loop.index), stmt);

	if (rc != SQLITE_OK) {
		return rc;
	}

\for (p in s.params) {
\call bind(t.columns | at(p.index), p.optional, loop.number, "statement")
\}
\if (s.kind == "get") {
	rc = sqlite3_step(stmt);

	if (rc == SQLITE_ROW) {
		rc = read(stmt, out);
		rc = rc == SQLITE_OK ? SQLITE_ROW : rc;
	}

	return detail::finish(stmt, rc);
\} \elif (s.kind == "insert") {
	rc = detail::run(stmt);
\if (not t.without_rowid) {

	if (rc == SQLITE_DONE && out_key) {
\if (t.id_column) {
		rc = detail::id(sqlite3_last_insert_rowid(ctx.db()), out_key->v);
		rc = rc == SQLITE_OK ? SQLITE_DONE : rc;
\} \else {
		*out_key = sqlite3_last_insert_rowid(ctx.db());
\}
	}
\}

	return detail::finish(stmt, rc == SQLITE_DONE ? SQLITE_OK : rc);
\} \else {
	rc = detail::run(stmt);

	if (rc == SQLITE_DONE) {
		rc = sqlite3_changes(ctx.db()) > 0 ? SQLITE_OK : SQLITE_DONE;
	}

	return detail::finish(stmt, rc);
\}
}
\}
\}

} // namespace \(t.words | cppname)
} // namespace \(prefix)
