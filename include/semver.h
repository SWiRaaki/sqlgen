#ifndef SQLGEN_SEMVER_H
#define SQLGEN_SEMVER_H

/**
 * @file semver.h
 * @brief Semantic Versioning 2.0.0: strict parsing and precedence.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief One parsed version. Every string is owned, and empty rather than NULL when absent.
 */
typedef struct {
	char     *text;         //!< The version exactly as written.
	uint64_t  major;
	uint64_t  minor;
	uint64_t  patch;
	char     *prerelease;   //!< The part after '-', without it; "" when there is none.
	char     *build;        //!< The part after '+', without it; "" when there is none.
} sg_semver;

/**
 * @brief Parse @p len bytes of @p text as a version.
 * @param error_at On failure, receives the byte offset into @p text where the problem starts.
 * @return false when the text is not a valid version. @p out is then untouched.
 *
 * Strict to the specification: no leading 'v', no leading zeros in a numeric component or in a
 * numeric pre-release identifier, no empty identifiers, only `[0-9A-Za-z-]` in identifiers, and
 * each numeric component must fit in 64 bits.
 */
bool sg_semver_parse(const char *text, size_t len, sg_semver *out, size_t *error_at);

/**
 * @brief Compare by precedence (specification section 11). Build metadata takes no part, so
 *        `1.0.0+a` and `1.0.0+b` compare equal.
 * @return Negative, zero or positive, as strcmp.
 */
int sg_semver_compare(const sg_semver *left, const sg_semver *right);

/** @brief Release the strings. */
void sg_semver_free(sg_semver *version);

#endif /* SQLGEN_SEMVER_H */
