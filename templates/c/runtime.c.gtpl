<<<
output: \(prefix).c
>>>
\include("defs")
\call header("every script", null)

#include "\(prefix)_internal.h"

#include <stdlib.h>
#include <string.h>

struct \(prefix)_ctx {
	sqlite3      *db;
	sqlite3_stmt *statements[\(prefix | upper)_STATEMENT_COUNT + 1];
};

int \(prefix)_ctx_open(\(prefix)_ctx **out, sqlite3 *db) {
	\(prefix)_ctx *ctx = calloc(1, sizeof *ctx);

	if (!ctx) {
		return SQLITE_NOMEM;
	}

	ctx->db = db;
	*out    = ctx;
	return SQLITE_OK;
}

void \(prefix)_ctx_close(\(prefix)_ctx *ctx) {
	if (!ctx) {
		return;
	}

	for (int i = 0; i < \(prefix | upper)_STATEMENT_COUNT; ++i) {
		sqlite3_finalize(ctx->statements[i]);
	}

	free(ctx);
}

sqlite3 *\(prefix)_ctx_db(const \(prefix)_ctx *ctx) {
	return ctx->db;
}

void \(prefix)_text_free(\(prefix)_text *text) {
	free(text->ptr);
	text->ptr = NULL;
	text->len = 0;
}

void \(prefix)_blob_free(\(prefix)_blob *blob) {
	free(blob->ptr);
	blob->ptr = NULL;
	blob->len = 0;
}

void \(prefix)_value_free(\(prefix)_value *value) {
	\(prefix)_text_free(&value->text);
	\(prefix)_blob_free(&value->blob);
	memset(value, 0, sizeof *value);
}

/* A statement is prepared once per connection and reused; each use starts it afresh. */
int \(prefix)__statement(\(prefix)_ctx *ctx, int slot, const char *sql, sqlite3_stmt **out) {
	sqlite3_stmt *stmt = ctx->statements[slot];

	if (!stmt) {
		int rc = sqlite3_prepare_v3(ctx->db, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, NULL);

		if (rc != SQLITE_OK) {
			return rc;
		}

		ctx->statements[slot] = stmt;
	} else {
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
	}

	*out = stmt;
	return SQLITE_OK;
}

/* A list whose statement is still stepping for another cursor gets a statement of its own. */
int \(prefix)__cursor(\(prefix)_ctx *ctx, int slot, const char *sql, \(prefix)_cursor *out) {
	sqlite3_stmt *stmt = ctx->statements[slot];
	int           rc;

	out->stmt  = NULL;
	out->owned = stmt && sqlite3_stmt_busy(stmt);
	rc = out->owned ? sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL)
			: \(prefix)__statement(ctx, slot, sql, &stmt);

	if (rc == SQLITE_OK) {
		out->stmt = stmt;
	}

	return rc;
}

int \(prefix)__cursor_fail(\(prefix)_cursor *cursor, int rc) {
	\(prefix)_cursor_close(cursor);
	return rc;
}

void \(prefix)_cursor_close(\(prefix)_cursor *cursor) {
	if (!cursor->stmt) {
		return;
	}

	if (cursor->owned) {
		sqlite3_finalize(cursor->stmt);
	} else {
		sqlite3_reset(cursor->stmt);
		sqlite3_clear_bindings(cursor->stmt);
	}

	cursor->stmt = NULL;
}

int \(prefix)__finish(sqlite3_stmt *stmt, int rc) {
	sqlite3_reset(stmt);
	sqlite3_clear_bindings(stmt);
	return rc;
}

/* Steps to the end, past any rows a RETURNING clause yields. */
int \(prefix)__run(sqlite3_stmt *stmt) {
	int rc;

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
	}

	return rc;
}

/* The statement must select exactly these columns, in this order. */
int \(prefix)__columns(sqlite3_stmt *stmt, const char *const *names, int count) {
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

int \(prefix)__id(sqlite3_int64 value, uint64_t *out) {
	if (value < 0) {
		return SQLITE_RANGE;
	}

	*out = (uint64_t)value;
	return SQLITE_OK;
}

int \(prefix)__read_text(sqlite3_stmt *stmt, int column, \(prefix)_text *out) {
	const unsigned char *text = sqlite3_column_text(stmt, column);
	size_t               len  = (size_t)sqlite3_column_bytes(stmt, column);

	if (!text && sqlite3_errcode(sqlite3_db_handle(stmt)) == SQLITE_NOMEM) {
		return SQLITE_NOMEM;
	}

	out->ptr = malloc(len + 1);

	if (!out->ptr) {
		return SQLITE_NOMEM;
	}

	if (len) {
		memcpy(out->ptr, text, len);
	}

	out->ptr[len] = '\0';
	out->len      = len;
	return SQLITE_OK;
}

int \(prefix)__read_blob(sqlite3_stmt *stmt, int column, \(prefix)_blob *out) {
	const void *bytes = sqlite3_column_blob(stmt, column);
	size_t      len   = (size_t)sqlite3_column_bytes(stmt, column);

	out->ptr = NULL;
	out->len = 0;

	if (len == 0) {
		return SQLITE_OK;
	}

	if (!bytes || !(out->ptr = malloc(len))) {
		return SQLITE_NOMEM;
	}

	memcpy(out->ptr, bytes, len);
	out->len = len;
	return SQLITE_OK;
}

int \(prefix)__read_value(sqlite3_stmt *stmt, int column, \(prefix)_value *out) {
	out->type = sqlite3_column_type(stmt, column);

	switch (out->type) {
	case SQLITE_INTEGER: out->integer = sqlite3_column_int64(stmt, column); return SQLITE_OK;
	case SQLITE_FLOAT:   out->real = sqlite3_column_double(stmt, column); return SQLITE_OK;
	case SQLITE_TEXT:    return \(prefix)__read_text(stmt, column, &out->text);
	case SQLITE_BLOB:    return \(prefix)__read_blob(stmt, column, &out->blob);
	default:             return SQLITE_OK;
	}
}

int \(prefix)__read_id(sqlite3_stmt *stmt, int column, uint64_t *out) {
	return \(prefix)__id(sqlite3_column_int64(stmt, column), out);
}

int \(prefix)__bind_text(sqlite3_stmt *stmt, int index, const char *text, size_t len) {
	return text ? sqlite3_bind_text64(stmt, index, text, (sqlite3_uint64)len, SQLITE_STATIC, SQLITE_UTF8)
			: sqlite3_bind_null(stmt, index);
}

int \(prefix)__bind_blob(sqlite3_stmt *stmt, int index, const void *bytes, size_t len) {
	return bytes ? sqlite3_bind_blob64(stmt, index, bytes, (sqlite3_uint64)len, SQLITE_STATIC)
			: sqlite3_bind_null(stmt, index);
}

int \(prefix)__bind_value(sqlite3_stmt *stmt, int index, const \(prefix)_value *value) {
	switch (value ? value->type : SQLITE_NULL) {
	case SQLITE_INTEGER: return sqlite3_bind_int64(stmt, index, (sqlite3_int64)value->integer);
	case SQLITE_FLOAT:   return sqlite3_bind_double(stmt, index, value->real);
	case SQLITE_TEXT:    return \(prefix)__bind_text(stmt, index, value->text.ptr, value->text.len);
	case SQLITE_BLOB:    return \(prefix)__bind_blob(stmt, index, value->blob.ptr ? (const void *)value->blob.ptr : "", value->blob.len);
	default:             return sqlite3_bind_null(stmt, index);
	}
}

int \(prefix)__bind_id(sqlite3_stmt *stmt, int index, uint64_t id) {
	if (id > (uint64_t)INT64_MAX) {
		return SQLITE_RANGE;
	}

	return sqlite3_bind_int64(stmt, index, (sqlite3_int64)id);
}
