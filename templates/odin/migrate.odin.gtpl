<<<
output: migrate.odin
when:   embed_scripts
>>>
\include("defs")
\call header("every script", null)
package \(prefix)

import "base:runtime"
import "core:fmt"

/* The tables migration keeps. Their layout never changes, so any version of a program reads what
 * any other wrote, whichever language it was generated in. */
@(private = "file")
TABLES_SQL :: "create table if not exists _sys_patch (\n" +
	"\tserial     integer primary key autoincrement,\n" +
	"\tmajor      integer not null,\n" +
	"\tminor      integer not null,\n" +
	"\tpatch      integer not null,\n" +
	"\tprerelease text    not null default '',\n" +
	"\tbuild      text    not null default '',\n" +
	"\tapplied_by text    not null,\n" +
	"\tcreated_at integer not null,\n" +
	"\tunique (major, minor, patch, prerelease)\n" +
	");\n" +
	"create table if not exists _sys_script (\n" +
	"\tserial     integer primary key autoincrement,\n" +
	"\tpatch      integer not null references _sys_patch (serial) on delete cascade,\n" +
	"\tordinal    integer not null,\n" +
	"\tname       text    not null,\n" +
	"\tsql        text    not null,\n" +
	"\tcreated_at integer not null,\n" +
	"\tunique (patch, ordinal)\n" +
	");\n"

/* Milliseconds since the epoch, computed by SQLite. */
@(private = "file")
NOW :: "cast((julianday('now') - 2440587.5) * 86400000 as integer)"

@(private = "file")
Migration :: struct {
	db:        ^Sqlite3,
	message:   string,
	allocator: runtime.Allocator,
}

@(private = "file")
fail :: proc(m: ^Migration, rc: Result, format: string, args: ..any) -> Result {
	if m.message == "" {
		m.message = fmt.aprintf(format, ..args, allocator = m.allocator)
	}

	return rc
}

@(private = "file")
exec :: proc(m: ^Migration, sql: cstring, what: string) -> Result {
	errmsg: cstring
	rc := Result(sqlite3_exec(m.db, sql, nil, nil, &errmsg))

	if rc != .OK {
		rc = fail(m, rc, "%s: %s", what, errmsg if errmsg != nil else sqlite3_errstr(i32(rc)))
	}

	sqlite3_free(rawptr(errmsg))
	return rc
}

@(private = "file")
prepare :: proc(m: ^Migration, sql: string) -> (^Sqlite3_Stmt, Result) {
	stmt: ^Sqlite3_Stmt
	rc := Result(sqlite3_prepare_v3(m.db, raw_data(sql), i32(len(sql)), 0, &stmt, nil))

	if rc != .OK {
		return nil, fail(m, rc, "%s", sqlite3_errmsg(m.db))
	}

	return stmt, .OK
}

@(private = "file")
column_text :: proc(stmt: ^Sqlite3_Stmt, column: i32) -> string {
	data := sqlite3_column_text(stmt, column)
	return string(data[:sqlite3_column_bytes(stmt, column)]) if data != nil else ""
}

/* The patch this program carries under that identity, or -1. Build metadata takes no part. */
@(private = "file")
find_patch :: proc(major, minor, patch: i64, prerelease: string) -> int {
	for p, i in patches {
		if i64(p.major) == major && i64(p.minor) == minor && i64(p.patch) == patch && p.prerelease == prerelease {
			return i
		}
	}

	return -1
}

/* What the database recorded for an applied patch must be what this program carries for it. */
@(private = "file")
verify :: proc(m: ^Migration, serial: i64, patch: Patch) -> Result {
	stmt, rc := prepare(m, "select ordinal, name, sql from _sys_script where patch = ?1 order by ordinal")

	if rc != .OK {
		return rc
	}

	defer sqlite3_finalize(stmt)
	sqlite3_bind_int64(stmt, 1, serial)
	seen := 0

	for rc == .OK && Result(sqlite3_step(stmt)) == .ROW {
		name := column_text(stmt, 1)

		if sqlite3_column_int64(stmt, 0) != i64(seen) || seen >= len(patch.scripts) {
			rc = fail(m, .ERROR, "patch %s: %s was applied, but this program does not carry it", patch.version, name)
		} else if name != string(patch.scripts[seen].name) {
			rc = fail(m, .ERROR, "patch %s: %s was applied where this program carries %s", patch.version, name,
				patch.scripts[seen].name)
		} else if column_text(stmt, 2) != string(patch.scripts[seen].sql) {
			rc = fail(m, .ERROR, "patch %s: %s changed since it was applied; an applied script stays as it ran, so a change belongs in a new patch",
				patch.version, name)
		}

		seen += 1
	}

	if rc == .OK && seen < len(patch.scripts) {
		rc = fail(m, .ERROR, "patch %s: %s was added after the patch was applied; a change belongs in a new patch",
			patch.version, patch.scripts[seen].name)
	}

	return rc
}

/* Marks every patch the database has in done, and finds the newest; each is verified. */
@(private = "file")
read_applied :: proc(m: ^Migration, done: []bool, newest: ^int) -> Result {
	stmt, rc := prepare(m, "select serial, major, minor, patch, prerelease from _sys_patch order by serial")

	if rc != .OK {
		return rc
	}

	defer sqlite3_finalize(stmt)

	for rc == .OK && Result(sqlite3_step(stmt)) == .ROW {
		major := sqlite3_column_int64(stmt, 1)
		minor := sqlite3_column_int64(stmt, 2)
		patch := sqlite3_column_int64(stmt, 3)
		prerelease := column_text(stmt, 4)
		at := find_patch(major, minor, patch, prerelease)

		if at < 0 {
			return fail(m, .ERROR, "the database has patch %d.%d.%d%s%s, which this program does not carry; a newer program migrated it",
				major, minor, patch, "-" if prerelease != "" else "", prerelease)
		}

		done[at] = true
		newest^ = max(newest^, at)
		rc = verify(m, sqlite3_column_int64(stmt, 0), patches[at])
	}

	return rc
}

@(private = "file")
record :: proc(m: ^Migration, patch: Patch, applied_by: string) -> Result {
	stmt, rc := prepare(m, "insert into _sys_patch (major, minor, patch, prerelease, build, applied_by, created_at) values (?1, ?2, ?3, ?4, ?5, ?6, " + NOW + ")")

	if rc != .OK {
		return rc
	}

	sqlite3_bind_int64(stmt, 1, i64(patch.major))
	sqlite3_bind_int64(stmt, 2, i64(patch.minor))
	sqlite3_bind_int64(stmt, 3, i64(patch.patch))
	bind_text(stmt, 4, patch.prerelease, false)
	bind_text(stmt, 5, patch.build, false)
	bind_text(stmt, 6, applied_by, false)
	rc = .OK if Result(sqlite3_step(stmt)) == .DONE else Result(sqlite3_errcode(m.db))
	sqlite3_finalize(stmt)

	serial := sqlite3_last_insert_rowid(m.db)

	if rc == .OK {
		stmt, rc = prepare(m, "insert into _sys_script (patch, ordinal, name, sql, created_at) values (?1, ?2, ?3, ?4, " + NOW + ")")
	}

	for script, i in patch.scripts {
		if rc != .OK {
			break
		}

		sqlite3_reset(stmt)
		sqlite3_bind_int64(stmt, 1, serial)
		sqlite3_bind_int64(stmt, 2, i64(i))
		bind_text(stmt, 3, string(script.name), false)
		bind_text(stmt, 4, string(script.sql), false)
		rc = .OK if Result(sqlite3_step(stmt)) == .DONE else Result(sqlite3_errcode(m.db))
	}

	sqlite3_finalize(stmt)
	return rc if rc == .OK else fail(m, rc, "recording patch %s: %s", patch.version, sqlite3_errmsg(m.db))
}

@(private = "file")
apply :: proc(m: ^Migration, patch: Patch, applied_by: string) -> Result {
	for script in patch.scripts {
		what := fmt.tprintf("patch %s, %s", patch.version, script.name)
		rc := exec(m, script.sql, what)

		if rc == .OK && sqlite3_get_autocommit(m.db) != 0 {
			rc = fail(m, .ERROR, "%s ended the migration's transaction itself (COMMIT, END or ROLLBACK); what ran before it may already be committed", what)
		}

		if rc != .OK {
			return rc
		}
	}

	return record(m, patch, applied_by)
}

/*
 * Brings db up to this program's schema, in one transaction: creates _sys_patch and _sys_script if
 * they are missing, runs the system-tables script, then applies every patch the database lacks, in
 * order, recording each with applied_by and the text of every script.
 *
 * Refused, with nothing changed: a patch the database has but this program does not carry (a newer
 * program migrated it); an applied script whose text changed, or that vanished or was added; a
 * missing patch older than one already applied.
 *
 * applied is how many patches ran; on failure, message says why, allocated with allocator.
 */
migrate :: proc(db: ^Sqlite3, applied_by: string, allocator := context.allocator) -> (applied: int, message: string, rc: Result) {
	m := Migration{db = db, allocator = allocator}
	done := make([]bool, len(patches), context.temp_allocator)
	newest := -1

	if rc = exec(&m, "begin immediate", "starting the migration"); rc != .OK {
		return 0, m.message, rc
	}

	rc = exec(&m, TABLES_SQL, "creating _sys_patch and _sys_script")

	if script, has := systables.?; has && rc == .OK {
		rc = exec(&m, script.sql, string(script.name))
	}

	if rc == .OK {
		rc = read_applied(&m, done, &newest)
	}

	for patch, i in patches {
		if rc != .OK {
			break
		}

		if done[i] {
			continue
		}

		if i < newest {
			rc = fail(&m, .ERROR, "patch %s is older than %s, which is already applied; patches apply in order, so it cannot run now",
				patch.version, patches[newest].version)
			break
		}

		if rc = apply(&m, patch, applied_by); rc == .OK {
			applied += 1
		}
	}

	if rc == .OK {
		rc = exec(&m, "commit", "committing the migration")
	}

	if rc != .OK {
		if sqlite3_get_autocommit(db) == 0 {
			sqlite3_exec(db, "rollback", nil, nil, nil)
		}

		return 0, m.message, rc
	}

	return applied, "", .OK
}
