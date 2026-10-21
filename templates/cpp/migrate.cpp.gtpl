<<<
output: \(prefix)_migrate.cpp
when:   embed_scripts
>>>
\include("defs")
\call header("every script", null)

#include "\(prefix).hpp"

#include <string.h>

namespace \(prefix) {
namespace {

/* The tables migration keeps. Their layout never changes, so any version of a program reads what
 * any other wrote -- the C output's included. */
const char tables_sql[] =
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

/* SQLITE_STATIC -- the text outlives the statement's use of it -- without the C cast the macro
 * spells it with. */
const sqlite3_destructor_type borrowed = nullptr;

/* Milliseconds since the epoch, computed by SQLite. */
const char now_sql[] = "cast((julianday('now') - 2440587.5) * 86400000 as integer)";

int fail(std::string *error, int rc, const std::string &message) {
	if (error) {
		*error = message;
	}

	return rc;
}

int run(sqlite3 *db, const char *sql, const std::string &what, std::string *error) {
	char *message = nullptr;
	int   rc      = sqlite3_exec(db, sql, nullptr, nullptr, &message);

	if (rc != SQLITE_OK) {
		rc = fail(error, rc, what + ": " + (message ? message : sqlite3_errstr(rc)));
	}

	sqlite3_free(message);
	return rc;
}

std::string text(sqlite3_stmt *stmt, int column) {
	const unsigned char *value = sqlite3_column_text(stmt, column);

	return value ? reinterpret_cast<const char *>(value) : "";
}

/* The patch this program carries under that identity, or -1. Build metadata takes no part. */
long find_patch(sqlite3_int64 major, sqlite3_int64 minor, sqlite3_int64 patch, const std::string &prerelease) {
	for (size_t i = 0; i < patch_count; ++i) {
		const Patch &p = patches[i];

		if (static_cast<sqlite3_int64>(p.major) == major && static_cast<sqlite3_int64>(p.minor) == minor
				&& static_cast<sqlite3_int64>(p.patch) == patch && prerelease == p.prerelease) {
			return static_cast<long>(i);
		}
	}

	return -1;
}

/* What the database recorded for an applied patch must be what this program carries for it. */
int verify(sqlite3 *db, sqlite3_int64 serial, const Patch &patch, std::string *error) {
	sqlite3_stmt *stmt = nullptr;
	size_t        seen = 0;
	int           rc   = sqlite3_prepare_v2(db, "select ordinal, name, sql from _sys_script where patch = ?1"
			" order by ordinal", -1, &stmt, nullptr);

	if (rc != SQLITE_OK) {
		return fail(error, rc, std::string("reading _sys_script: ") + sqlite3_errmsg(db));
	}

	sqlite3_bind_int64(stmt, 1, serial);

	while (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
		std::string version = patch.version;
		std::string name    = text(stmt, 1);

		if (sqlite3_column_int64(stmt, 0) != static_cast<sqlite3_int64>(seen) || seen >= patch.count) {
			rc = fail(error, SQLITE_ERROR, "patch " + version + ": " + name + " was applied, but this program"
					" does not carry it");
		} else if (name != patch.scripts[seen].name) {
			rc = fail(error, SQLITE_ERROR, "patch " + version + ": " + name + " was applied where this program"
					" carries " + patch.scripts[seen].name);
		} else if (text(stmt, 2) != patch.scripts[seen].sql) {
			rc = fail(error, SQLITE_ERROR, "patch " + version + ": " + name + " changed since it was applied;"
					" an applied script stays as it ran, so a change belongs in a new patch");
		}

		++seen;
	}

	sqlite3_finalize(stmt);

	if (rc == SQLITE_OK && seen < patch.count) {
		rc = fail(error, SQLITE_ERROR, std::string("patch ") + patch.version + ": " + patch.scripts[seen].name
				+ " was added after the patch was applied; a change belongs in a new patch");
	}

	return rc;
}

/* Marks every patch the database has in done, and finds the newest; each is verified. */
int read_applied(sqlite3 *db, std::vector<bool> &done, long &newest, std::string *error) {
	sqlite3_stmt *stmt = nullptr;
	int           rc   = sqlite3_prepare_v2(db, "select serial, major, minor, patch, prerelease"
			" from _sys_patch order by serial", -1, &stmt, nullptr);

	if (rc != SQLITE_OK) {
		return fail(error, rc, std::string("reading _sys_patch: ") + sqlite3_errmsg(db));
	}

	while (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
		sqlite3_int64 major      = sqlite3_column_int64(stmt, 1);
		sqlite3_int64 minor      = sqlite3_column_int64(stmt, 2);
		sqlite3_int64 patch      = sqlite3_column_int64(stmt, 3);
		std::string   prerelease = text(stmt, 4);
		long          at         = find_patch(major, minor, patch, prerelease);

		if (at < 0) {
			rc = fail(error, SQLITE_ERROR, "the database has patch " + std::to_string(major) + "."
					+ std::to_string(minor) + "." + std::to_string(patch) + (prerelease.empty() ? "" : "-")
					+ prerelease + ", which this program does not carry; a newer program migrated it");
			break;
		}

		done[static_cast<size_t>(at)] = true;
		newest = at > newest ? at : newest;
		rc     = verify(db, sqlite3_column_int64(stmt, 0), patches[at], error);
	}

	sqlite3_finalize(stmt);
	return rc;
}

int record(sqlite3 *db, const Patch &patch, const std::string &applied_by, std::string *error) {
	sqlite3_stmt *stmt = nullptr;
	std::string   sql  = std::string("insert into _sys_patch (major, minor, patch, prerelease, build,"
			" applied_by, created_at) values (?1, ?2, ?3, ?4, ?5, ?6, ") + now_sql + ")";
	int           rc   = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(patch.major));
		sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(patch.minor));
		sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(patch.patch));
		sqlite3_bind_text(stmt, 4, patch.prerelease, -1, borrowed);
		sqlite3_bind_text(stmt, 5, patch.build, -1, borrowed);
		sqlite3_bind_text(stmt, 6, applied_by.c_str(), -1, borrowed);
		rc = sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(db);
	}

	sqlite3_finalize(stmt);
	stmt = nullptr;

	sqlite3_int64 serial = sqlite3_last_insert_rowid(db);

	if (rc == SQLITE_OK) {
		sql = std::string("insert into _sys_script (patch, ordinal, name, sql, created_at)"
				" values (?1, ?2, ?3, ?4, ") + now_sql + ")";
		rc  = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
	}

	for (size_t i = 0; rc == SQLITE_OK && i < patch.count; ++i) {
		sqlite3_reset(stmt);
		sqlite3_bind_int64(stmt, 1, serial);
		sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(i));
		sqlite3_bind_text(stmt, 3, patch.scripts[i].name, -1, borrowed);
		sqlite3_bind_text(stmt, 4, patch.scripts[i].sql, -1, borrowed);
		rc = sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : sqlite3_errcode(db);
	}

	sqlite3_finalize(stmt);
	return rc == SQLITE_OK ? rc
			: fail(error, rc, std::string("recording patch ") + patch.version + ": " + sqlite3_errmsg(db));
}

int apply(sqlite3 *db, const Patch &patch, const std::string &applied_by, std::string *error) {
	for (size_t i = 0; i < patch.count; ++i) {
		std::string what = std::string("patch ") + patch.version + ", " + patch.scripts[i].name;
		int         rc   = run(db, patch.scripts[i].sql, what, error);

		if (rc == SQLITE_OK && sqlite3_get_autocommit(db)) {
			rc = fail(error, SQLITE_ERROR, what + " ended the migration's transaction itself (COMMIT, END or"
					" ROLLBACK); what ran before it may already be committed");
		}

		if (rc != SQLITE_OK) {
			return rc;
		}
	}

	return record(db, patch, applied_by, error);
}

} // namespace

int migrate(sqlite3 *db, const std::string &applied_by, size_t *applied, std::string *error) {
	std::vector<bool> done(patch_count, false);
	long              newest = -1;
	size_t            count  = 0;
	int               rc;

	if (error) {
		error->clear();
	}

	if (applied) {
		*applied = 0;
	}

	if ((rc = run(db, "begin immediate", "starting the migration", error)) != SQLITE_OK) {
		return rc;
	}

	rc = run(db, tables_sql, "creating _sys_patch and _sys_script", error);

	if (rc == SQLITE_OK && systables) {
		rc = run(db, systables->sql, systables->name, error);
	}

	if (rc == SQLITE_OK) {
		rc = read_applied(db, done, newest, error);
	}

	for (size_t i = 0; rc == SQLITE_OK && i < patch_count; ++i) {
		if (done[i]) {
			continue;
		}

		if (static_cast<long>(i) < newest) {
			rc = fail(error, SQLITE_ERROR, std::string("patch ") + patches[i].version + " is older than "
					+ patches[newest].version + ", which is already applied; patches apply in order, so it"
					" cannot run now");
			break;
		}

		if ((rc = apply(db, patches[i], applied_by, error)) == SQLITE_OK) {
			++count;
		}
	}

	if (rc == SQLITE_OK) {
		rc = run(db, "commit", "committing the migration", error);
	}

	if (rc != SQLITE_OK) {
		if (!sqlite3_get_autocommit(db)) {
			sqlite3_exec(db, "rollback", nullptr, nullptr, nullptr);
		}

		return rc;
	}

	if (applied) {
		*applied = count;
	}

	return SQLITE_OK;
}

} // namespace \(prefix)
