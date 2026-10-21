<<<
output: \(prefix).cpp
>>>
\include("defs")
\call header("every script", null)

#include "\(prefix)_internal.hpp"

namespace \(prefix) {

Context::Context(sqlite3 *db) : db_(db), statements_(detail::STATEMENT_COUNT + 1, nullptr) {
}

Context::~Context() {
	for (size_t i = 0; i < statements_.size(); ++i) {
		sqlite3_finalize(statements_[i]);
	}
}

sqlite3 *Context::db() const {
	return db_;
}

CursorBase::CursorBase() : stmt_(nullptr), owned_(false) {
}

CursorBase::~CursorBase() {
	close();
}

void CursorBase::close() {
	if (!stmt_) {
		return;
	}

	if (owned_) {
		sqlite3_finalize(stmt_);
	} else {
		sqlite3_reset(stmt_);
		sqlite3_clear_bindings(stmt_);
	}

	stmt_ = nullptr;
}

namespace detail {

/* SQLITE_STATIC -- the bytes outlive the statement's use of them -- without the C cast the macro
 * spells it with. */
static const sqlite3_destructor_type borrowed = nullptr;

/* A statement is prepared once per connection and reused; each use starts it afresh. */
int Access::statement(Context &ctx, int slot, const char *sql, sqlite3_stmt *&out) {
	sqlite3_stmt *&stmt = ctx.statements_[static_cast<size_t>(slot)];

	if (!stmt) {
		int rc = sqlite3_prepare_v3(ctx.db_, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, nullptr);

		if (rc != SQLITE_OK) {
			return rc;
		}
	} else {
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}

	out = stmt;
	return SQLITE_OK;
}

/* A list whose statement is still stepping for another cursor gets a statement of its own. */
int Access::cursor(Context &ctx, int slot, const char *sql, CursorBase &out) {
	sqlite3_stmt *stmt = ctx.statements_[static_cast<size_t>(slot)];
	int           rc;

	out.close();
	out.owned_ = stmt && sqlite3_stmt_busy(stmt);
	rc = out.owned_ ? sqlite3_prepare_v2(ctx.db_, sql, -1, &stmt, nullptr) : statement(ctx, slot, sql, stmt);

	if (rc == SQLITE_OK) {
		out.stmt_ = stmt;
	}

	return rc;
}

int finish(sqlite3_stmt *stmt, int rc) {
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	return rc;
}

/* Steps to the end, past any rows a RETURNING clause yields. */
int run(sqlite3_stmt *stmt) {
	int rc;

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
	}

	return rc;
}

/* The statement must select exactly these columns, in this order. */
int columns(sqlite3_stmt *stmt, const char *const *names, int count) {
	if (sqlite3_column_count(stmt) != count) {
		return SQLITE_MISMATCH;
	}

	for (int i = 0; i < count; ++i) {
		const char *name = sqlite3_column_name(stmt, i);

		if (!name || sqlite3_stricmp(name, names[i]) != 0) {
			return SQLITE_MISMATCH;
		}
	}

	return SQLITE_OK;
}

int id(sqlite3_int64 value, uint64_t &out) {
	if (value < 0) {
		return SQLITE_RANGE;
	}

	out = static_cast<uint64_t>(value);
	return SQLITE_OK;
}

int read_text(sqlite3_stmt *stmt, int column, std::string &out) {
	const unsigned char *text = sqlite3_column_text(stmt, column);
	size_t               len  = static_cast<size_t>(sqlite3_column_bytes(stmt, column));

	if (!text && sqlite3_errcode(sqlite3_db_handle(stmt)) == SQLITE_NOMEM) {
		return SQLITE_NOMEM;
	}

	out.assign(text ? reinterpret_cast<const char *>(text) : "", len);
	return SQLITE_OK;
}

int read_blob(sqlite3_stmt *stmt, int column, std::vector<unsigned char> &out) {
	const unsigned char *bytes = static_cast<const unsigned char *>(sqlite3_column_blob(stmt, column));
	size_t               len   = static_cast<size_t>(sqlite3_column_bytes(stmt, column));

	if (!bytes && len) {
		return SQLITE_NOMEM;
	}

	out.assign(bytes, bytes + len);
	return SQLITE_OK;
}

int read_value(sqlite3_stmt *stmt, int column, Value &out) {
	out.type = sqlite3_column_type(stmt, column);

	switch (out.type) {
	case SQLITE_INTEGER: out.integer = sqlite3_column_int64(stmt, column); return SQLITE_OK;
	case SQLITE_FLOAT:   out.real = sqlite3_column_double(stmt, column); return SQLITE_OK;
	case SQLITE_TEXT:    return read_text(stmt, column, out.text);
	case SQLITE_BLOB:    return read_blob(stmt, column, out.blob);
	default:             return SQLITE_OK;
	}
}

int read_id(sqlite3_stmt *stmt, int column, uint64_t &out) {
	return id(sqlite3_column_int64(stmt, column), out);
}

/* A cursor goes on stepping after the call that bound it returns -- often with its arguments'
 * temporaries gone -- so it takes a copy; a statement that finishes within the call reads the
 * caller's bytes. */
static sqlite3_destructor_type keep(bool copy) {
	return copy ? reinterpret_cast<sqlite3_destructor_type>(static_cast<intptr_t>(-1)) : borrowed;
}

int bind_text(sqlite3_stmt *stmt, int index, const std::string &text, bool copy) {
	return sqlite3_bind_text64(stmt, index, text.data(), static_cast<sqlite3_uint64>(text.size()), keep(copy),
			SQLITE_UTF8);
}

int bind_blob(sqlite3_stmt *stmt, int index, const std::vector<unsigned char> &bytes, bool copy) {
	/* An empty blob is still a blob, not NULL: it needs a pointer that is not null. */
	static const unsigned char none = 0;

	return sqlite3_bind_blob64(stmt, index, bytes.empty() ? &none : &bytes[0],
			static_cast<sqlite3_uint64>(bytes.size()), keep(copy));
}

int bind_value(sqlite3_stmt *stmt, int index, const Value &value, bool copy) {
	switch (value.type) {
	case SQLITE_INTEGER: return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value.integer));
	case SQLITE_FLOAT:   return sqlite3_bind_double(stmt, index, value.real);
	case SQLITE_TEXT:    return bind_text(stmt, index, value.text, copy);
	case SQLITE_BLOB:    return bind_blob(stmt, index, value.blob, copy);
	default:             return sqlite3_bind_null(stmt, index);
	}
}

int bind_id(sqlite3_stmt *stmt, int index, uint64_t id) {
	if (id > static_cast<uint64_t>(INT64_MAX)) {
		return SQLITE_RANGE;
	}

	return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(id));
}

} // namespace detail
} // namespace \(prefix)
