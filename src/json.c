#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

enum { MAX_DEPTH = 64 };

typedef struct {
	const char      *text;
	size_t           len;
	size_t           at;
	int              depth;
	sg_json_problem *problem;
} parser;

static bool fail(parser *p, size_t at, const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	vsnprintf(p->problem->reason, sizeof p->problem->reason, fmt, args);
	va_end(args);

	p->problem->offset = at;
	return false;
}

static int peek(const parser *p) {
	return p->at < p->len ? (unsigned char)p->text[p->at] : -1;
}

static void skip_space(parser *p) {
	while (p->at < p->len) {
		char c = p->text[p->at];

		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
			return;
		}

		++p->at;
	}
}

static bool is_identifier_start(int c) {
	return c == '_' || c == '$' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* The reason for an unexpected byte, naming the JSON5 feature it most likely belongs to. */
static bool unexpected(parser *p, const char *expected) {
	int c = peek(p);

	if (c < 0) {
		return fail(p, p->len, "the file ends where %s was expected", expected);
	}

	if (c == '/') {
		return fail(p, p->at, "comments are not allowed in JSON");
	}

	if (c == '\'') {
		return fail(p, p->at, "strings must be in double quotes");
	}

	if (c >= 0x20 && c < 0x7F) {
		return fail(p, p->at, "'%c' where %s was expected", c, expected);
	}

	return fail(p, p->at, "byte 0x%02X where %s was expected", (unsigned)c);
}

/* The length of the well-formed UTF-8 sequence at p->at, or 0 (RFC 3629, table 3-7 of Unicode). */
static size_t utf8_length(const parser *p) {
	const unsigned char *s    = (const unsigned char *)p->text + p->at;
	size_t               left = p->len - p->at;
	unsigned char        lo   = 0x80;
	unsigned char        hi   = 0xBF;
	size_t               n;

	if (s[0] >= 0xC2 && s[0] <= 0xDF) {
		n = 2;
	} else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
		n  = 3;
		lo = s[0] == 0xE0 ? 0xA0 : 0x80;
		hi = s[0] == 0xED ? 0x9F : 0xBF;
	} else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
		n  = 4;
		lo = s[0] == 0xF0 ? 0x90 : 0x80;
		hi = s[0] == 0xF4 ? 0x8F : 0xBF;
	} else {
		return 0;
	}

	if (left < n || s[1] < lo || s[1] > hi) {
		return 0;
	}

	for (size_t i = 2; i < n; ++i) {
		if (s[i] < 0x80 || s[i] > 0xBF) {
			return 0;
		}
	}

	return n;
}

static bool is_hex(int c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* At the opening quote. Leaves p->at after the closing one. */
static bool string(parser *p) {
	size_t open = p->at++;

	for (;;) {
		int c = peek(p);

		if (c < 0) {
			return fail(p, open, "this string is never closed");
		}

		if (c == '"') {
			++p->at;
			return true;
		}

		if (c < 0x20) {
			return fail(p, p->at, "a control character inside a string must be escaped");
		}

		if (c == '\\') {
			++p->at;
			c = peek(p);

			if (c == 'u') {
				for (int i = 1; i <= 4; ++i) {
					if (p->at + (size_t)i >= p->len || !is_hex((unsigned char)p->text[p->at + (size_t)i])) {
						return fail(p, p->at - 1, "\\u must be followed by four hexadecimal digits");
					}
				}

				p->at += 5;
				continue;
			}

			if (c < 0 || !strchr("\"\\/bfnrt", c)) {
				return fail(p, p->at - 1, "not a JSON escape; use \\\" \\\\ \\/ \\b \\f \\n \\r \\t or \\uXXXX");
			}

			++p->at;
			continue;
		}

		if (c >= 0x80) {
			size_t n = utf8_length(p);

			if (n == 0) {
				return fail(p, p->at, "not valid UTF-8");
			}

			p->at += n;
			continue;
		}

		++p->at;
	}
}

static bool digits(parser *p) {
	size_t from = p->at;

	while (peek(p) >= '0' && peek(p) <= '9') {
		++p->at;
	}

	return p->at > from;
}

static bool number(parser *p) {
	size_t start = p->at;

	if (peek(p) == '-') {
		++p->at;
	}

	if (peek(p) == '0') {
		++p->at;

		if (peek(p) >= '0' && peek(p) <= '9') {
			return fail(p, start, "a number must not have a leading zero");
		}
	} else if (!digits(p)) {
		return fail(p, start, "not a JSON number");
	}

	if (peek(p) == '.') {
		++p->at;

		if (!digits(p)) {
			return fail(p, start, "a number needs digits after its decimal point");
		}
	}

	if (peek(p) == 'e' || peek(p) == 'E') {
		++p->at;

		if (peek(p) == '+' || peek(p) == '-') {
			++p->at;
		}

		if (!digits(p)) {
			return fail(p, start, "a number needs digits in its exponent");
		}
	}

	return true;
}

static bool word(parser *p, const char *literal) {
	size_t n = strlen(literal);

	if (p->len - p->at < n || memcmp(p->text + p->at, literal, n) != 0) {
		return false;
	}

	p->at += n;
	return true;
}

static bool value(parser *p);

typedef struct {
	size_t start;
	size_t len;
} span;

typedef SG_VEC(span) spans;

static bool members(parser *p, spans *keys);

static bool object(parser *p) {
	spans keys = {0};
	bool  ok   = members(p, &keys);

	free(keys.items);
	return ok;
}

/* At the opening brace; every key seen so far is kept, so a repeat is found wherever it is. */
static bool members(parser *p, spans *keys) {
	++p->at;
	skip_space(p);

	if (peek(p) == '}') {
		++p->at;
		return true;
	}

	for (;;) {
		int c = peek(p);

		if (c != '"') {
			if (is_identifier_start(c)) {
				return fail(p, p->at, "keys must be strings in double quotes");
			}

			return unexpected(p, "a key in double quotes");
		}

		size_t start = p->at;

		if (!string(p)) {
			return false;
		}

		/* Compared as written: an escaped spelling of the same key is not caught. */
		span key = { start + 1, p->at - start - 2 };

		for (size_t i = 0; i < keys->count; ++i) {
			const span *seen = &keys->items[i];

			if (seen->len == key.len && memcmp(p->text + seen->start, p->text + key.start, key.len) == 0) {
				return fail(p, start, "the key %.*s appears twice in this object",
						(int)(key.len + 2), p->text + start);
			}
		}

		*SG_PUSH(*keys) = key;

		skip_space(p);

		if (peek(p) != ':') {
			return unexpected(p, "':' after the key");
		}

		++p->at;
		skip_space(p);

		if (!value(p)) {
			return false;
		}

		skip_space(p);

		if (peek(p) == '}') {
			++p->at;
			return true;
		}

		if (peek(p) != ',') {
			return unexpected(p, "',' or '}'");
		}

		size_t comma = p->at++;

		skip_space(p);

		if (peek(p) == '}') {
			return fail(p, comma, "a trailing comma is not allowed in JSON");
		}
	}
}

static bool array(parser *p) {
	++p->at;
	skip_space(p);

	if (peek(p) == ']') {
		++p->at;
		return true;
	}

	for (;;) {
		if (!value(p)) {
			return false;
		}

		skip_space(p);

		if (peek(p) == ']') {
			++p->at;
			return true;
		}

		if (peek(p) != ',') {
			return unexpected(p, "',' or ']'");
		}

		size_t comma = p->at++;

		skip_space(p);

		if (peek(p) == ']') {
			return fail(p, comma, "a trailing comma is not allowed in JSON");
		}
	}
}

static bool value(parser *p) {
	int  c = peek(p);
	bool ok;

	if (++p->depth > MAX_DEPTH) {
		return fail(p, p->at, "nested more than %d levels deep", MAX_DEPTH);
	}

	if (c == '{') {
		ok = object(p);
	} else if (c == '[') {
		ok = array(p);
	} else if (c == '"') {
		ok = string(p);
	} else if (c == '-' || (c >= '0' && c <= '9')) {
		ok = number(p);
	} else if (word(p, "true") || word(p, "false") || word(p, "null")) {
		ok = true;
	} else {
		ok = unexpected(p, "a value");
	}

	--p->depth;
	return ok;
}

bool sg_json_check(const char *text, size_t len, sg_json_problem *problem) {
	parser p = { text, len, 0, 0, problem };

	if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) {
		return fail(&p, 0, "a byte order mark is not allowed in JSON");
	}

	skip_space(&p);

	if (!value(&p)) {
		return false;
	}

	skip_space(&p);

	if (p.at < len) {
		return peek(&p) == '/' ? unexpected(&p, "nothing")
				: fail(&p, p.at, "more text after the end of the JSON value");
	}

	return true;
}
