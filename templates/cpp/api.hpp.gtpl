<<<
output: \(prefix).hpp
>>>
\include("defs")
\call header("every script", null)

#ifndef \(prefix | upper)_HPP
#define \(prefix | upper)_HPP

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include <sqlite3.h>

/*
 * Every function returns an SQLite result code:
 *   SQLITE_ROW       a get or a one-row query found its row; a cursor stepped to its next row
 *   SQLITE_DONE      a get or a one-row query found no row; a cursor has no more rows; a write
 *                    changed no row
 *   SQLITE_OK        a write changed at least one row; anything else succeeded
 *   SQLITE_MISMATCH  a statement handed to a read function does not select that row's columns
 *   SQLITE_RANGE     an id outside what both uint64_t and SQLite's int64 can hold
 *   anything else    the error SQLite reported
 *
 * Rows are plain aggregates: a row starts value-initialised (Row row = Row();), every function
 * filling one replaces all of it, and it owns nothing but its std::string and std::vector members.
 * Memory runs out the way it does for any std::string: std::bad_alloc.
 */
namespace \(prefix) {

/* A value that may be NULL. An aggregate, so a row holding some stays one: a value-initialised
 * nullable has no value. */
template <typename T>
struct nullable {
	T    value;
	bool has_value;

	explicit operator bool() const { return has_value; }
	const T &operator*() const { return value; }
	T &operator*() { return value; }
	const T *operator->() const { return &value; }
	T *operator->() { return &value; }
	T value_or(const T &fallback) const { return has_value ? value : fallback; }
};

/* A nullable holding v; {} is one holding nothing. */
template <typename T>
nullable<T> some(const T &v) {
	nullable<T> n = { v, true };
	return n;
}

/* A nullable text from a literal. */
inline nullable<std::string> some(const char *v) {
	nullable<std::string> n = { v, true };
	return n;
}

/* A column without a fixed type: type is SQLITE_INTEGER, SQLITE_FLOAT, SQLITE_TEXT, SQLITE_BLOB or
 * SQLITE_NULL, and says which member holds the value. 0, as value-initialised, is NULL too. */
struct Value {
	int                        type;
	int64_t                    integer;
	double                     real;
	std::string                text;
	std::vector<unsigned char> blob;
};
\for (t in tables) {
\if (t.id_column) {

/* The primary key of \(t.name): a type of its own, so an id of another table does not fit. */
struct \(t | idtype) {
	uint64_t v;

	explicit operator uint64_t() const { return v; }
};

inline bool operator==(\(t | idtype) a, \(t | idtype) b) { return a.v == b.v; }
inline bool operator!=(\(t | idtype) a, \(t | idtype) b) { return a.v != b.v; }
inline bool operator<(\(t | idtype) a, \(t | idtype) b) { return a.v < b.v; }
inline bool operator<=(\(t | idtype) a, \(t | idtype) b) { return a.v <= b.v; }
inline bool operator>(\(t | idtype) a, \(t | idtype) b) { return a.v > b.v; }
inline bool operator>=(\(t | idtype) a, \(t | idtype) b) { return a.v >= b.v; }
\}
\}

namespace detail {
struct Access;
}

/* One connection's prepared statements. It does not own the connection. */
class Context {
public:
	explicit Context(sqlite3 *db);
	~Context();

	sqlite3 *db() const;

private:
	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;

	sqlite3                    *db_;
	std::vector<sqlite3_stmt *> statements_;

	friend struct detail::Access;
};

/* What a cursor of any row type holds: the statement being stepped. */
class CursorBase {
public:
	CursorBase();
	~CursorBase();

	/* Ends the cursor before its last row; it is done anyway when it is destroyed. */
	void close();

	/* The statement being stepped; null before a list or query opened the cursor, and after close. */
	sqlite3_stmt *statement() const { return stmt_; }

protected:
	sqlite3_stmt *stmt_;

private:
	CursorBase(const CursorBase &) = delete;
	CursorBase &operator=(const CursorBase &) = delete;

	bool owned_;

	friend struct detail::Access;
};

/* The rows of a list or a query, each read into a Row. */
template <typename Row>
class Cursor : public CursorBase {
public:
	/* SQLITE_ROW with out filled, SQLITE_DONE after the last row, or an error. */
	int next(Row &out) {
		if (!stmt_) {
			return SQLITE_MISUSE;
		}

		int rc = sqlite3_step(stmt_);

		if (rc != SQLITE_ROW) {
			return rc;
		}

		rc = read(stmt_, out);
		return rc == SQLITE_OK ? SQLITE_ROW : rc;
	}
};
\if (embed_scripts) {

/* One schema script, its text exactly as it runs. */
struct Script {
	const char *name;
	const char *sql;
};

/* One patch: its version, parsed, and its scripts in the order they run. */
struct Patch {
	const char   *version;
	uint64_t      major;
	uint64_t      minor;
	uint64_t      patch;
	const char   *prerelease;
	const char   *build;
	const Script *scripts;
	size_t        count;
};

/* The script creating the system tables, run before every patch; null when there is none. */
extern const Script *const systables;

/* Every patch, in the order of their versions' precedence. */
extern const Patch *const patches;
extern const size_t patch_count;

/* A hash of every script's name and text, in order: it changes whenever the schema does. */
extern const char schema_hash[];

/*
 * Brings db up to this program's schema, in one transaction: creates _sys_patch and _sys_script if
 * they are missing, runs the system-tables script, then applies every patch the database lacks, in
 * order, recording each with applied_by and the text of every script.
 *
 * Refused, with nothing changed: a patch the database has but this program does not carry (a newer
 * program migrated it); an applied script whose text changed, or that vanished or was added; a
 * missing patch older than one already applied.
 *
 * *applied receives how many patches ran; on failure *error receives a message. Either may be null.
 */
int migrate(sqlite3 *db, const std::string &applied_by, size_t *applied, std::string *error);
\}

} // namespace \(prefix)
\for (t in tables) {
\if (t.id_column) {

namespace std {

template <>
struct hash<\(prefix)::\(t | idtype)> {
	size_t operator()(\(prefix)::\(t | idtype) id) const { return hash<uint64_t>()(id.v); }
};

} // namespace std
\}
\}

\for (t in tables) {
#include "\(prefix)_\(t.words | snake).hpp"
\}
\for (q in queries) {
#include "\(prefix)_\(q.words | snake).hpp"
\}

#endif
