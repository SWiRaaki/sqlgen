#ifndef SQLGEN_JSONW_H
#define SQLGEN_JSONW_H

/**
 * @file jsonw.h
 * @brief A small JSON pretty-printer: tabs for indentation, one member per line, except inside a
 *        container opened flat, which keeps all of its members on one line.
 */

#include "model.h"
#include "util.h"

enum { SG_JW_MAX_DEPTH = 16 };

typedef struct {
	sg_buf *out;
	int     depth;
	bool    first[SG_JW_MAX_DEPTH];
	bool    flat[SG_JW_MAX_DEPTH];
} sg_jw;

/** @brief Open an object (`{`) or array (`[`); @p key is NULL inside an array or at the top. */
void sg_jw_open(sg_jw *w, const char *key, char brace, bool flat);

/** @brief Close the innermost container with @p brace. */
void sg_jw_close(sg_jw *w, char brace);

/** @brief A string member; NULL writes null. */
void sg_jw_text(sg_jw *w, const char *key, const char *value);

void sg_jw_bool(sg_jw *w, const char *key, bool value);
void sg_jw_int(sg_jw *w, const char *key, long long value);
void sg_jw_null(sg_jw *w, const char *key);

/** @brief An array of strings, on one line. */
void sg_jw_names(sg_jw *w, const char *key, const sg_names *list);

#endif /* SQLGEN_JSONW_H */
