<<<
output: \(prefix)_\(q.words | snake).hpp
each:   q in queries
>>>
\include("defs")
\call header(q.path, null)

#ifndef \(prefix | upper)_\(q.words | upper)_HPP
#define \(prefix | upper)_\(q.words | upper)_HPP

#include "\(prefix).hpp"

namespace \(prefix) {
namespace \(q.words | cppname) {
\if (q.row_table) {

/* It selects exactly the columns of \(q.row_table), so its rows are that table's. */
typedef \(q.row_words | cppname)::Row Row;
\} \elif (q.rows != "none") {

/* One row of \(q.name). */
struct Row {
\call members(q.columns)
};

/* Fills out from the current row of a statement selecting what \(q.name) selects. */
int read(sqlite3_stmt *stmt, Row &out);
\}

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
 * Opens a cursor over its rows.
\} \elif (q.rows == "exactly_one") {
 * Fills out with its one row: SQLITE_ROW.
\} \elif (q.rows == "one") {
 * Fills out with its row: SQLITE_ROW, or SQLITE_DONE when there is none.
\} \elif (q.writes) {
 * SQLITE_OK when it changed a row, SQLITE_DONE when it changed none.
\} \else {
 * SQLITE_OK when it ran.
\}
 */
\call signature(q);

} // namespace \(q.words | cppname)
} // namespace \(prefix)

#endif
