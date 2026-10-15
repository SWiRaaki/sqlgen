#include "semver.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

static bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

static bool is_ident_char(char c) {
	return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

static bool parse_number(const char *text, size_t len, uint64_t *out) {
	uint64_t value = 0;

	if (len == 0 || (len > 1 && text[0] == '0')) {
		return false;
	}

	for (size_t i = 0; i < len; ++i) {
		if (!is_digit(text[i])) {
			return false;
		}

		uint64_t digit = (uint64_t)(text[i] - '0');

		if (value > (UINT64_MAX - digit) / 10) {
			return false;
		}

		value = value * 10 + digit;
	}

	*out = value;
	return true;
}

/* Dot-separated identifiers; numeric ones may not have leading zeros in a pre-release. */
static bool valid_identifiers(const char *text, size_t len, bool prerelease, size_t *bad) {
	size_t start = 0;

	for (size_t at = 0; at <= len; ++at) {
		if (at < len && text[at] != '.') {
			if (!is_ident_char(text[at])) {
				*bad = at;
				return false;
			}

			continue;
		}

		size_t part    = at - start;
		bool   numeric = part > 0;

		for (size_t i = start; i < at; ++i) {
			numeric = numeric && is_digit(text[i]);
		}

		if (part == 0 || (prerelease && numeric && part > 1 && text[start] == '0')) {
			*bad = start;
			return false;
		}

		start = at + 1;
	}

	return true;
}

bool sg_semver_parse(const char *text, size_t len, sg_semver *out, size_t *error_at) {
	const char *plus  = memchr(text, '+', len);
	size_t      head  = plus ? (size_t)(plus - text) : len;
	const char *minus = memchr(text, '-', head);
	size_t      core  = minus ? (size_t)(minus - text) : head;
	uint64_t    parts[3];
	size_t      start = 0;
	size_t      bad   = 0;

	for (int i = 0; i < 3; ++i) {
		size_t end = start;

		while (end < core && text[end] != '.') {
			++end;
		}

		if ((i < 2 && end == core) || (i == 2 && end != core)
				|| !parse_number(text + start, end - start, &parts[i])) {
			*error_at = start;
			return false;
		}

		start = end + 1;
	}

	if (minus && !valid_identifiers(minus + 1, head - core - 1, true, &bad)) {
		*error_at = core + 1 + bad;
		return false;
	}

	if (plus && !valid_identifiers(plus + 1, len - head - 1, false, &bad)) {
		*error_at = head + 1 + bad;
		return false;
	}

	out->text       = sg_xstrndup(text, len);
	out->major      = parts[0];
	out->minor      = parts[1];
	out->patch      = parts[2];
	out->prerelease = minus ? sg_xstrndup(minus + 1, head - core - 1) : sg_xstrdup("");
	out->build      = plus ? sg_xstrndup(plus + 1, len - head - 1) : sg_xstrdup("");
	return true;
}

static int compare_u64(uint64_t left, uint64_t right) {
	return left < right ? -1 : (left > right ? 1 : 0);
}

static bool all_digits(const char *text, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		if (!is_digit(text[i])) {
			return false;
		}
	}

	return len > 0;
}

static int compare_identifier(const char *left, size_t llen, const char *right, size_t rlen) {
	bool lnum = all_digits(left, llen);
	bool rnum = all_digits(right, rlen);

	if (lnum && rnum) {
		/* No leading zeros, so the longer is the larger, and equal lengths compare as text. This
		 * also holds for identifiers too long for any integer type. */
		if (llen != rlen) {
			return llen < rlen ? -1 : 1;
		}

		return memcmp(left, right, llen);
	}

	if (lnum != rnum) {
		return lnum ? -1 : 1;
	}

	size_t shared = llen < rlen ? llen : rlen;
	int    order  = memcmp(left, right, shared);

	if (order != 0) {
		return order;
	}

	return compare_u64(llen, rlen);
}

int sg_semver_compare(const sg_semver *left, const sg_semver *right) {
	int order = compare_u64(left->major, right->major);

	if (order == 0) {
		order = compare_u64(left->minor, right->minor);
	}

	if (order == 0) {
		order = compare_u64(left->patch, right->patch);
	}

	if (order != 0) {
		return order;
	}

	bool lpre = left->prerelease[0] != '\0';
	bool rpre = right->prerelease[0] != '\0';

	if (lpre != rpre) {
		return lpre ? -1 : 1;
	}

	const char *l = left->prerelease;
	const char *r = right->prerelease;

	while (*l && *r) {
		size_t llen = strcspn(l, ".");
		size_t rlen = strcspn(r, ".");

		order = compare_identifier(l, llen, r, rlen);

		if (order != 0) {
			return order;
		}

		l += llen + (l[llen] == '.');
		r += rlen + (r[rlen] == '.');
	}

	return *l ? 1 : (*r ? -1 : 0);
}

void sg_semver_free(sg_semver *version) {
	free(version->text);
	free(version->prerelease);
	free(version->build);
	memset(version, 0, sizeof *version);
}
