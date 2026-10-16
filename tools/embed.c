/* embed <output.c> <language> <file.gtpl>... -- compile templates into sqlgen as a built-in set.
 *
 * Each template becomes a byte array rather than a string literal: C99 only promises string
 * literals of 4095 characters, and -pedantic holds the build to that. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *base_name(const char *path) {
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fputs("usage: embed <output.c> <language> <file.gtpl>...\n", stderr);
		return 2;
	}

	FILE *out = fopen(argv[1], "w");

	if (!out) {
		perror(argv[1]);
		return 1;
	}

	fprintf(out, "/* The built-in %s templates, compiled in by tools/embed.c from templates/%s. */\n\n"
			"#include \"builtin.h\"\n\n", argv[2], argv[2]);

	for (int i = 3; i < argc; ++i) {
		FILE *in = fopen(argv[i], "rb");
		int   c;
		long  n = 0;

		if (!in) {
			perror(argv[i]);
			fclose(out);
			remove(argv[1]);
			return 1;
		}

		fprintf(out, "static const char text_%d[] = {", i - 3);

		while ((c = fgetc(in)) != EOF) {
			/* Bytes above 127 (UTF-8 in a template) do not fit a signed char without the cast. */
			fprintf(out, c > 127 ? "%s(char)0x%02x," : "%s0x%02x,", n++ % 16 ? " " : "\n\t", (unsigned)c);
		}

		fprintf(out, "\n\t0x00\n};\n\n");
		fclose(in);
	}

	fprintf(out, "static const sg_template_source templates[] = {\n");

	for (int i = 3; i < argc; ++i) {
		fprintf(out, "\t{ \"%s\", text_%d },\n", base_name(argv[i]), i - 3);
	}

	fprintf(out, "};\n\nconst sg_builtin_set sg_builtin_%s = { \"%s\", templates, %d };\n", argv[2], argv[2],
			argc - 3);

	if (fclose(out) != 0) {
		perror(argv[1]);
		return 1;
	}

	return 0;
}
