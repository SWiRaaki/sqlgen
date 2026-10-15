#ifndef SQLGEN_JSON_H
#define SQLGEN_JSON_H

/**
 * @file json.h
 * @brief Strict JSON validation (RFC 8259), reporting where the first problem is and what it is.
 *
 * sqlgen reads its configuration through SQLite's JSON functions, which also accept JSON5:
 * comments, unquoted keys, single quotes, trailing commas. The file must be plain JSON so that any
 * other tool can read it too, so this check runs first and refuses everything JSON5 adds -- with a
 * reason naming the JSON5 feature, since that is the likely mistake.
 *
 * Beyond the grammar it refuses a byte order mark (RFC 8259 section 8.1 forbids adding one),
 * malformed UTF-8 (section 8.1 requires UTF-8), and a key appearing twice in one object, which
 * section 4 leaves to the reader and SQLite would silently resolve.
 */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	size_t offset;        //!< Byte offset of the problem.
	char   reason[160];   //!< What is wrong there.
} sg_json_problem;

/**
 * @brief Check that @p len bytes of @p text are one strict JSON value.
 * @param problem Receives the first problem; untouched when the text is valid.
 * @return true when the text is valid.
 */
bool sg_json_check(const char *text, size_t len, sg_json_problem *problem);

#endif /* SQLGEN_JSON_H */
