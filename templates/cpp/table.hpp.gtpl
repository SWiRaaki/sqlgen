<<<
output: \(prefix)_\(t.words | snake).hpp
each:   t in tables
>>>
\include("defs")
\call header(t.origin.script, t.origin.patch)

#ifndef \(prefix | upper)_\(t.words | upper)_HPP
#define \(prefix | upper)_\(t.words | upper)_HPP

#include "\(prefix).hpp"

namespace \(prefix) {
namespace \(t.words | cppname) {

/* One row of \(t.name). */
struct Row {
\call members(t.columns)
};

/* Fills out from the current row of a statement selecting exactly \(t.select_list). */
int read(sqlite3_stmt *stmt, Row &out);
\for (s in t.statements) {

/* \(s.sql) */
\if (s.kind == "get") {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		Row &out);
\} \elif (s.kind == "list") {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		Cursor<Row> &out);
\} \elif (s.kind == "insert" and not t.without_rowid) {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\},
		\if (t.id_column) {\(t | idtype)\} \else {int64_t\} *out_key);
\} \else {
int \(s.words | cppname)(Context &ctx\for (p in s.params) {,
		\call param(t.columns | at(p.index), p.optional)\});
\}
\}

} // namespace \(t.words | cppname)
} // namespace \(prefix)

#endif
