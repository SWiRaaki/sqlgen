/* Semantic Versioning 2.0.0: what parses, what does not, and the precedence of section 11. */

#include <stdio.h>
#include <string.h>

#include "semver.h"

static int failures;

static void parses(const char *text, int expected) {
	sg_semver version;
	size_t    at = 0;
	int       ok = sg_semver_parse(text, strlen(text), &version, &at);

	if (ok != expected) {
		printf("semver: '%s' %s, expected it %s\n", text, ok ? "parsed" : "was refused",
				expected ? "to parse" : "to be refused");
		++failures;
	}

	if (ok) {
		sg_semver_free(&version);
	}
}

static int compare(const char *left, const char *right) {
	sg_semver l;
	sg_semver r;
	size_t    at;

	sg_semver_parse(left, strlen(left), &l, &at);
	sg_semver_parse(right, strlen(right), &r, &at);

	int order = sg_semver_compare(&l, &r);

	sg_semver_free(&l);
	sg_semver_free(&r);
	return order;
}

int main(void) {
	static const char *const valid[] = {
		"0.0.0", "1.2.3", "10.20.30", "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-0.3.7",
		"1.0.0-x.7.z.92", "1.0.0-x-y-z.--", "1.0.0+20130313144700", "1.0.0-beta+exp.sha.5114f85",
		"1.0.0+21AF26D3----117B344092BD", "1.0.0+001", "18446744073709551615.0.0", NULL
	};

	static const char *const invalid[] = {
		"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "v1.2.3", "1.2.3-", "1.2.3+",
		"1.2.3-01", "1.2.3-a..b", "1.2.3-a_b", "1.2.3+a..b", "1.2.3 ", " 1.2.3", "-1.2.3",
		"18446744073709551616.0.0", "1.2.3-+", NULL
	};

	/* Section 11's example, in ascending order. */
	static const char *const ascending[] = {
		"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
		"1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.1.0", "2.0.0", "10.0.0", NULL
	};

	for (size_t i = 0; valid[i]; ++i) {
		parses(valid[i], 1);
	}

	for (size_t i = 0; invalid[i]; ++i) {
		parses(invalid[i], 0);
	}

	for (size_t i = 0; ascending[i]; ++i) {
		for (size_t j = 0; ascending[j]; ++j) {
			int order    = compare(ascending[i], ascending[j]);
			int expected = i < j ? -1 : (i > j ? 1 : 0);

			if ((order < 0 ? -1 : (order > 0 ? 1 : 0)) != expected) {
				printf("semver: %s vs %s gave %d, expected %d\n", ascending[i], ascending[j], order,
						expected);
				++failures;
			}
		}
	}

	if (compare("1.0.0+a", "1.0.0+b") != 0 || compare("1.0.0-rc.1+a", "1.0.0-rc.1") != 0) {
		puts("semver: build metadata took part in precedence");
		++failures;
	}

	/* Numeric identifiers longer than any integer still compare as numbers. */
	if (compare("1.0.0-99999999999999999999999", "1.0.0-100000000000000000000000") >= 0) {
		puts("semver: long numeric identifiers compared as text");
		++failures;
	}

	printf("semver: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
