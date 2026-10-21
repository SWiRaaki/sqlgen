<<<
output: \(prefix)_internal.hpp
>>>
\include("defs")
\call header("every script", null)

#ifndef \(prefix | upper)_INTERNAL_HPP
#define \(prefix | upper)_INTERNAL_HPP

/* What the generated sources share; not part of the interface. */

#include "\(prefix).hpp"

namespace \(prefix) {
namespace detail {

enum {
\for (t in tables) {
\for (s in t.statements) {
	STMT_\(t.words | upper)_\(s.words | upper),
\}
\}
\for (q in queries) {
	QUERY_\(q.words | upper),
\}
	STATEMENT_COUNT
};

struct Access {
	static int statement(Context &ctx, int slot, const char *sql, sqlite3_stmt *&out);
	static int cursor(Context &ctx, int slot, const char *sql, CursorBase &out);
};

int finish(sqlite3_stmt *stmt, int rc);
int run(sqlite3_stmt *stmt);
int columns(sqlite3_stmt *stmt, const char *const *names, int count);
int id(sqlite3_int64 value, uint64_t &out);

int read_text(sqlite3_stmt *stmt, int column, std::string &out);
int read_blob(sqlite3_stmt *stmt, int column, std::vector<unsigned char> &out);
int read_value(sqlite3_stmt *stmt, int column, Value &out);
int read_id(sqlite3_stmt *stmt, int column, uint64_t &out);

int bind_text(sqlite3_stmt *stmt, int index, const std::string &text, bool copy);
int bind_blob(sqlite3_stmt *stmt, int index, const std::vector<unsigned char> &bytes, bool copy);
int bind_value(sqlite3_stmt *stmt, int index, const Value &value, bool copy);
int bind_id(sqlite3_stmt *stmt, int index, uint64_t id);

} // namespace detail
} // namespace \(prefix)

#endif
