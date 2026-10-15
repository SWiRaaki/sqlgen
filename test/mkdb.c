/* mkdb <database> <script> [key] -- create a database file from a script, for the `report` tests.
 * The key needs a build with CIPHER=1. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

int main(int argc, char **argv) {
	sqlite3 *db      = NULL;
	char    *message = NULL;
	FILE    *file;
	long     size;
	char    *text;

	if (argc != 3 && argc != 4) {
		fputs("usage: mkdb <database> <script> [key]\n", stderr);
		return 2;
	}

	file = fopen(argv[2], "rb");

	if (!file || fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
		perror(argv[2]);
		return 1;
	}

	text = malloc((size_t)size + 1);

	if (!text || fread(text, 1, (size_t)size, file) != (size_t)size) {
		perror(argv[2]);
		return 1;
	}

	text[size] = '\0';
	fclose(file);
	remove(argv[1]);

	if (sqlite3_open(argv[1], &db) != SQLITE_OK) {
		fprintf(stderr, "%s: %s\n", argv[1], sqlite3_errmsg(db));
		return 1;
	}

	if (argc == 4) {
#ifdef SQLITE_HAS_CODEC
		sqlite3_key(db, argv[3], (int)strlen(argv[3]));
#else
		fputs("mkdb: a key needs a build with CIPHER=1\n", stderr);
		return 1;
#endif
	}

	if (sqlite3_exec(db, text, NULL, NULL, &message) != SQLITE_OK) {
		fprintf(stderr, "%s: %s\n", argv[1], message ? message : sqlite3_errmsg(db));
		return 1;
	}

	sqlite3_close(db);
	free(text);
	return 0;
}
