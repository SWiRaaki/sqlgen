#ifndef SQLGEN_UTIL_H
#define SQLGEN_UTIL_H

/**
 * @file util.h
 * @brief Diagnostics, allocation, growable buffers and arrays, files and paths.
 *
 * sqlgen is a short-lived command-line tool, so running out of memory is not recovered from: every
 * allocation here either succeeds or ends the process with a message. That keeps every caller free
 * of a failure path that could only ever lead to the same exit.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define SG_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#define SG_NORETURN __attribute__((noreturn))
#else
#define SG_PRINTF(fmt, args)
#define SG_NORETURN
#endif

/**
 * @brief Report an error in an input file and count it.
 * @param file The file it concerns, or NULL when it concerns no file.
 * @param line 1-based line, or 0 when no position is known.
 * @param col  1-based column in bytes, or 0 when only the line is known.
 *
 * Printed as `file:line:col: error: message`, the form editors and terminals make clickable.
 */
void sg_error(const char *file, unsigned line, unsigned col, const char *fmt, ...) SG_PRINTF(4, 5);

/**
 * @brief Report a warning. The same form as @ref sg_error; it does not count as an error.
 */
void sg_warning(const char *file, unsigned line, unsigned col, const char *fmt, ...) SG_PRINTF(4, 5);

/**
 * @brief How many errors have been reported so far.
 */
unsigned sg_error_count(void);

/** @brief malloc that never returns NULL; a zero size still yields a unique pointer. */
void *sg_xmalloc(size_t size);

/** @brief realloc that never returns NULL. */
void *sg_xrealloc(void *ptr, size_t size);

/** @brief strdup that never returns NULL; NULL in gives NULL out. */
char *sg_xstrdup(const char *text);

/** @brief Copy exactly @p len bytes and terminate them. */
char *sg_xstrndup(const char *text, size_t len);

/** @brief printf into a fresh allocation. */
char *sg_format(const char *fmt, ...) SG_PRINTF(1, 2);

/**
 * @brief Grow an array's storage, for @ref SG_PUSH.
 * @return The reallocated storage; @p cap is updated. Ends the process on overflow.
 */
void *sg_grow(void *items, size_t *cap, size_t size);

/**
 * @brief Declare a growable array of @p T: `SG_VEC(int) numbers;`, zero-initialised to empty.
 */
#define SG_VEC(T) struct { T *items; size_t count; size_t cap; }

/**
 * @brief Append a zeroed element to an @ref SG_VEC and yield a pointer to it.
 *
 * The pointer is valid until the next push to the same array.
 */
#define SG_PUSH(v)                                                                  \
	((v).count == (v).cap                                                           \
		? (void)((v).items = sg_grow((v).items, &(v).cap, sizeof *(v).items))       \
		: (void)0,                                                                  \
	 memset(&(v).items[(v).count], 0, sizeof *(v).items),                          \
	 &(v).items[(v).count++])

/**
 * @brief A growable byte buffer, always kept NUL-terminated once anything was appended.
 */
typedef struct {
	char   *data;
	size_t  len;
	size_t  cap;
} sg_buf;

void sg_buf_append(sg_buf *buf, const char *bytes, size_t len);
void sg_buf_puts(sg_buf *buf, const char *text);
void sg_buf_putc(sg_buf *buf, char c);
void sg_buf_printf(sg_buf *buf, const char *fmt, ...) SG_PRINTF(2, 3);
void sg_buf_free(sg_buf *buf);

/**
 * @brief Read a whole file.
 * @param out Receives the bytes, NUL-terminated; the caller frees it.
 * @param len Receives the length, not counting the terminator.
 * @return false, with an error reported, when the file cannot be read.
 */
bool sg_read_file(const char *path, char **out, size_t *len);

/**
 * @brief Write a whole file, replacing it.
 * @return false, with an error reported, on failure.
 */
bool sg_write_file(const char *path, const char *bytes, size_t len);

/**
 * @brief Create a file that must not exist yet, and write it.
 * @return false, with an error reported, when it exists already or cannot be written. The check and
 *         the creation are one operation (O_EXCL), so a file appearing in between is never replaced.
 */
bool sg_create_file(const char *path, const char *bytes, size_t len);

/** @brief Whether anything at all exists at @p path. */
bool sg_exists(const char *path);

/** @brief Whether @p path names something that exists and is a regular file. */
bool sg_is_file(const char *path);

/** @brief Whether @p path names a directory. */
bool sg_is_dir(const char *path);

/**
 * @brief Join a directory and a relative path. An absolute @p name, or a @p dir of "" or ".",
 *        returns a copy of @p name unchanged, so paths stay as short as the user wrote them.
 */
char *sg_path_join(const char *dir, const char *name);

/** @brief Everything before the last '/', or "." when there is none. */
char *sg_dirname(const char *path);

/**
 * @brief Translate a byte offset into a 1-based line and byte column.
 */
void sg_line_col(const char *text, size_t offset, unsigned *line, unsigned *col);

/** @brief Case-insensitive ASCII comparison, as SQLite compares identifiers. */
int sg_strcasecmp(const char *a, const char *b);

/** @brief Whether @p needle occurs in @p haystack, ASCII case-insensitively. */
bool sg_contains_nocase(const char *haystack, const char *needle);

#endif /* SQLGEN_UTIL_H */
