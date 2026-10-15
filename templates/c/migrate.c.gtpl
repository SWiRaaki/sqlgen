<<<
output: \(prefix)_migrate.c
when:   embed_scripts
>>>
\include("defs")
\call header("every script", null)

#include "\(prefix).h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The tables migration keeps. Their layout never changes, so any version of a program reads what
 * any other wrote. */
static const char tables_sql[] =
	"create table if not exists _sys_patch (\n"
	"\tserial     integer primary key autoincrement,\n"
	"\tmajor      integer not null,\n"
	"\tminor      integer not null,\n"
	"\tpatch      integer not null,\n"
	"\tprerelease text    not null default '',\n"
	"\tbuild      text    not null default '',\n"
	"\tapplied_by text    not null,\n"
	"\tcreated_at integer not null,\n"
	"\tunique (major, minor, patch, prerelease)\n"
	");\n"
	"create table if not exists _sys_script (\n"
	"\tserial     integer primary key autoincrement,\n"
	"\tpatch      integer not null references _sys_patch (serial) on delete cascade,\n"
	"\tordinal    integer not null,\n"
	"\tname       text    not null,\n"
	"\tsql        text    not null,\n"
	"\tcreated_at integer not null,\n"
	"\tunique (patch, ordinal)\n"
	");\n";

/* Milliseconds since the epoch, computed by SQLite. */
#define NOW "cast((julianday('now') - 2440587.5) * 86400000 as integer)"

static int fail(char **error, int rc, const char *format, ...) {
	if (error) {
		va_list args;

		va_start(args, format);
		sqlite3_free(*error);
		*error = sqlite3_vmprintf(format, args);
		va_end(args);
	}

	return rc;
}

static int run(sqlite3 *db, const char *sql, const char *what, char **error) {
	char *message = NULL;
	int   rc      = sqlite3_exec(db, sql, NULL, NULL, &message);

	if (rc != SQLITE_OK) {
		rc = fail(error, rc, "%s: %s", what, message ? message : sqlite3_errstr(rc));
	}

	sqlite3_free(message);
	return rc;
}

/* The patch this program carries under that identity, or -1. Build metadata takes no part. */
static long find_patch(sqlite3_int64 major, sqlite3_int64 minor, sqlite3_int64 patch, const char *prerelease) {
	for (size_t i = 0; i < \(prefix)_patch_count; ++i) {
		const \(prefix)_patch *p = &\(prefix)_patches[i];

		if ((sqlite3_int64)p->major == major && (sqlite3_int64)p->minor == minor
				&& (sqlite3_int64)p->patch == patch && strcmp(p->prerelease, prerelease) == 0) {
			return (long)i;
		}
	}

	return -1;
}

/* What the database recorded for an applied patch must be what this program carries for it. */
static int verify(sqlite3 *db, sqlite3_int64 serial, const \(prefix)_patch *patch, char **error) {
	sqlite3_stmt *stmt = NULL;
	size_t        seen = 0;
	int           rc   = sqlite3_prepare_v2(db, "select ordinal, name, sql from _sys_script where patch = ?1"
			" order by ordinal", -1, &stmt, NULL);

	if (rc != SQLITE_OK) {
		return fail(error, rc, "reading _sys_script: %s", sqlite3_errmsg(db));
	}

	sqlite3_bind_int64(stmt, 1, serial);

	while (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 1);
		const char *sql  = (const char *)sqlite3_column_text(stmt, 2);

		if (sqlite3_column_int64(stmt, 0) != (sqlite3_int64)seen || seen >= patch->count) {
			rc = fail(error, SQLITE_ERROR, "patch %s: %s was applied, but this program does not carry it",
					patch->version, name);
		} else if (strcmp(name, patch->scripts[seen].name) != 0) {
			rc = fail(error, SQLITE_ERROR, "patch %s: %s was applied where this program carries %s",
					patch->version, name, patch->scripts[seen].name);
		} else if (strcmp(sql, patch->scripts[seen].sql) != 0) {
			rc = fail(error, SQLITE_ERROR, "patch %s: %s changed since it was applied; an applied script"
					" stays as it ran, so a change belongs in a new patch", patch->version, name);
		}

		++seen;
	}

	sqlite3_finalize(stmt);

	if (rc == SQLITE_OK && seen < patch->count) {
		rc = fail(error, SQLITE_ERROR, "patch %s: %s was added after the patch was applied; a change"
				" belongs in a new patch", patch->version, patch->scripts[seen].name);
	}

	return rc;
}

/* Marks every patch the database has in done, and finds the newest; each is verified. */
static int read_applied(sqlite3 *db, unsigned char *done, long *newest, char **error) {
	sqlite3_stmt *stmt = NULL;
	int           rc   = sqlite3_prepare_v2(db, "select serial, major, minor, patch, prerelease"
			" from _sys_patch order by serial", -1, &stmt, NULL);

	if (rc != SQLITE_OK) {
		return fail(error, rc, "reading _sys_patch: %s", sqlite3_errmsg(db));
	}

	while (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
		sqlite3_int64 major      = sqlite3_column_int64(stmt, 1);
		sqlite3_int64 minor      = sqlite3_column_int64(stmt, 2);
		sqlite3_int64 patch      = sqlite3_column_int64(stmt, 3);
		const char   *prerelease = (const char *)sqlite3_column_text(stmt, 4);
		long          at         = find_patch(major, minor, patch, prerelease);

		if (at < 0) {
			rc = fail(error, SQLITE_ERROR, "the database has patch %lld.%lld.%lld%s%s, which this program"
					" does not carry; a newer program migrated it", (long long)major, (long long)minor,
					(long long)patch, prerelease[0] ? "-" : "", prerelease);
			break;
		}

		done[at] = 1;
		*newest  = at > *newest ? at : *newest;
		rc       = verify(db, sqlite3_column_int64(stmt, 0), &\(prefix)_patches[at], error);
	}

	sqlite3_finalize(stmt);
	return rc;
}

static int record(sqlite3 *db, const \(prefix)_patch *patch, const char *applied_by, char **error) {
	sqlite3_stmt *stmt = NULL;
	int           rc   = sqlite3_prepare_v2(db, "insert into _sys_patch (major, minor, patch, prerelease,"
			" build, applied_by, created_at) values (?1, ?2, ?3, ?4, ?5, ?6, " NOW ")", -1, &stmt, NULL);

	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)patch->major);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)patch->minor);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)patch->patch);
		sqlite3_bind_text(stmt, 4, patch->prerelease, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 5, patch->build, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 6, applied_by ? applied_by : "", -1, SQLITE_STATIC);
		rc = sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(db);
	}

	sqlite3_finalize(stmt);

	sqlite3_int64 serial = sqlite3_last_insert_rowid(db);

	if (rc == SQLITE_OK) {
		rc = sqlite3_prepare_v2(db, "insert into _sys_script (patch, ordinal, name, sql, created_at)"
				" values (?1, ?2, ?3, ?4, " NOW ")", -1, &stmt, NULL);
	}

	for (size_t i = 0; rc == SQLITE_OK && i < patch->count; ++i) {
		sqlite3_reset(stmt);
		sqlite3_bind_int64(stmt, 1, serial);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)i);
		sqlite3_bind_text(stmt, 3, patch->scripts[i].name, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 4, patch->scripts[i].sql, -1, SQLITE_STATIC);
		rc = sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(db);
	}

	sqlite3_finalize(stmt);
	return rc == SQLITE_OK ? rc : fail(error, rc, "recording patch %s: %s", patch->version, sqlite3_errmsg(db));
}

static int apply(sqlite3 *db, const \(prefix)_patch *patch, const char *applied_by, char **error) {
	for (size_t i = 0; i < patch->count; ++i) {
		char *what = sqlite3_mprintf("patch %s, %s", patch->version, patch->scripts[i].name);
		int   rc   = what ? run(db, patch->scripts[i].sql, what, error) : SQLITE_NOMEM;

		if (rc == SQLITE_OK && sqlite3_get_autocommit(db)) {
			rc = fail(error, SQLITE_ERROR, "%s ended the migration's transaction itself (COMMIT, END or"
					" ROLLBACK); what ran before it may already be committed", what);
		}

		sqlite3_free(what);

		if (rc != SQLITE_OK) {
			return rc;
		}
	}

	return record(db, patch, applied_by, error);
}

int \(prefix)_migrate(sqlite3 *db, const char *applied_by, size_t *applied, char **error) {
	unsigned char *done   = NULL;
	long           newest = -1;
	size_t         count  = 0;
	int            rc;

	if (error) {
		*error = NULL;
	}

	if (applied) {
		*applied = 0;
	}

	if ((rc = run(db, "begin immediate", "starting the migration", error)) != SQLITE_OK) {
		return rc;
	}

	rc = run(db, tables_sql, "creating _sys_patch and _sys_script", error);

	if (rc == SQLITE_OK && \(prefix)_systables) {
		rc = run(db, \(prefix)_systables->sql, \(prefix)_systables->name, error);
	}

	if (rc == SQLITE_OK && !(done = calloc(\(prefix)_patch_count + 1, 1))) {
		rc = fail(error, SQLITE_NOMEM, "out of memory");
	}

	if (rc == SQLITE_OK) {
		rc = read_applied(db, done, &newest, error);
	}

	for (size_t i = 0; rc == SQLITE_OK && i < \(prefix)_patch_count; ++i) {
		if (done[i]) {
			continue;
		}

		if ((long)i < newest) {
			rc = fail(error, SQLITE_ERROR, "patch %s is older than %s, which is already applied; patches"
					" apply in order, so it cannot run now", \(prefix)_patches[i].version,
					\(prefix)_patches[newest].version);
			break;
		}

		if ((rc = apply(db, &\(prefix)_patches[i], applied_by, error)) == SQLITE_OK) {
			++count;
		}
	}

	free(done);

	if (rc == SQLITE_OK) {
		rc = run(db, "commit", "committing the migration", error);
	}

	if (rc != SQLITE_OK) {
		if (!sqlite3_get_autocommit(db)) {
			sqlite3_exec(db, "rollback", NULL, NULL, NULL);
		}

		return rc;
	}

	if (applied) {
		*applied = count;
	}

	return SQLITE_OK;
}
