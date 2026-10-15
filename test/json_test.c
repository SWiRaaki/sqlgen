/* Strict JSON: what passes, and for what fails, where and why. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "util.h"

static int failures;

static void valid(const char *text) {
	sg_json_problem problem;

	if (!sg_json_check(text, strlen(text), &problem)) {
		printf("json: refused valid %s: %s at %zu\n", text, problem.reason, problem.offset);
		++failures;
	}
}

static void invalid_n(const char *label, const char *text, size_t len, size_t offset, const char *reason) {
	sg_json_problem problem;

	if (sg_json_check(text, len, &problem)) {
		printf("json: accepted %s\n", label);
		++failures;
	} else if (problem.offset != offset || !strstr(problem.reason, reason)) {
		printf("json: %s gave \"%s\" at %zu, expected \"%s\" at %zu\n", label, problem.reason,
				problem.offset, reason, offset);
		++failures;
	}
}

static void invalid(const char *text, size_t offset, const char *reason) {
	invalid_n(text, text, strlen(text), offset, reason);
}

int main(void) {
	valid("{}");
	valid("[]");
	valid("\"x\"");
	valid("0");
	valid("-0.5e+10");
	valid("1E5");
	valid("true");
	valid(" \t\r\n null \n");
	valid("{\"a\": [1, 2, {\"b\": null}], \"c\": true, \"d\": \"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u00e9\"}");
	valid("{\"\xC3\xA9\": \"\xE2\x82\xAC \xF0\x9F\x98\x80 \xF4\x8F\xBF\xBF\"}");
	valid("{\"a\": 1, \"b\": {\"a\": 2}}");   /* the same key in different objects */

	/* JSON5, each feature named. */
	invalid("{a: 1}", 1, "keys must be strings in double quotes");
	invalid("{\"a\": 1, // note\n\"b\": 2}", 9, "comments are not allowed");
	invalid("{\"a\": 1 /* note */}", 8, "comments are not allowed");
	invalid("{\"a\": 1} // note", 9, "comments are not allowed");
	invalid("{\"a\": 1,}", 7, "a trailing comma");
	invalid("[1, 2,]", 5, "a trailing comma");
	invalid("{\"a\": 'x'}", 6, "strings must be in double quotes");
	invalid("{'a': 1}", 1, "strings must be in double quotes");
	invalid("{\"a\": .5}", 6, "'.' where a value");
	invalid("{\"a\": +1}", 6, "'+' where a value");
	invalid("{\"a\": NaN}", 6, "'N' where a value");
	invalid("{\"a\": 0x10}", 7, "'x' where ',' or '}'");

	/* Strict JSON's own rules. */
	invalid("{\"a\": 1, \"a\": 2}", 9, "the key \"a\" appears twice");
	invalid("{\"a\": 01}", 6, "leading zero");
	invalid("{\"a\": 1.}", 6, "digits after its decimal point");
	invalid("{\"a\": 1e}", 6, "digits in its exponent");
	invalid("{\"a\": -}", 6, "not a JSON number");
	invalid("\"\\x\"", 1, "not a JSON escape");
	invalid("\"\\u12g4\"", 1, "four hexadecimal digits");
	invalid("\"a\tb\"", 2, "control character");
	invalid("\"abc", 0, "never closed");
	invalid("{\"a\": 1} x", 9, "more text after the end");
	invalid("", 0, "the file ends where a value");
	invalid("{\"a\": 1", 7, "the file ends where ',' or '}'");
	invalid("[1 2]", 3, "where ',' or ']'");
	invalid("{\"a\" 1}", 5, "':' after the key");
	invalid("tru", 0, "'t' where a value");
	invalid("\xEF\xBB\xBF{}", 0, "byte order mark");

	/* UTF-8 must be well formed: truncated, overlong, a surrogate, beyond U+10FFFF, a stray byte. */
	invalid("\"\xC3\x28\"", 1, "not valid UTF-8");
	invalid("\"\xC0\xAF\"", 1, "not valid UTF-8");
	invalid("\"\xE0\x80\xAF\"", 1, "not valid UTF-8");
	invalid("\"\xED\xA0\x80\"", 1, "not valid UTF-8");
	invalid("\"\xF4\x90\x80\x80\"", 1, "not valid UTF-8");
	invalid("\"\x80\"", 1, "not valid UTF-8");
	invalid_n("a truncated sequence at the end", "\"\xE2\x82", 3, 1, "not valid UTF-8");

	/* A NUL is a control character, not the end of the text. */
	invalid_n("an embedded NUL", "\"a\0b\"", 5, 2, "control character");

	/* Nesting is bounded, so a hostile file cannot exhaust the stack. */
	{
		char deep[70];

		memset(deep, '[', 66);
		deep[66] = '\0';
		invalid(deep, 64, "nested more than 64 levels");
	}

	/* A repeated key is found however many keys come before it. */
	{
		sg_buf many = {0};

		sg_buf_putc(&many, '{');

		for (int i = 0; i < 300; ++i) {
			sg_buf_printf(&many, "\"k%d\": %d, ", i, i);
		}

		size_t at = many.len;

		sg_buf_puts(&many, "\"k0\": 0}");
		invalid_n("a repeat after 300 keys", many.data, many.len, at, "appears twice");
		sg_buf_free(&many);
	}

	printf("json: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
