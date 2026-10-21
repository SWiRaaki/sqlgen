/*
 * Drives the Odin generated for test/cases/plan, its query files included, against a real
 * database: every kind of statement and query, the result codes, Maybe values, cursors, typed ids,
 * the embedded scripts and migration. The database is opened with the package's own bindings, and
 * everything runs on a tracking allocator: whatever a row or the migration allocates and nothing
 * frees is a failure.
 */
package plan_test

import "core:fmt"
import "core:mem"
import "core:os"
import "core:strings"
import db "../../cases/generate-odin/gen"

failures := 0

check :: proc(ok: bool, what: string, loc := #caller_location) {
	if !ok {
		fmt.printfln("plan_test.odin:%d: %s", loc.line, what)
		failures += 1
	}
}

fresh :: proc() -> ^db.Sqlite3 {
	conn: ^db.Sqlite3

	check(db.sqlite3_open_v2(":memory:", &conn, db.SQLITE_OPEN_READWRITE | db.SQLITE_OPEN_CREATE, nil) == 0, "open")
	check(db.sqlite3_exec(conn, "pragma foreign_keys = on", nil, nil, nil) == 0, "foreign keys")
	return conn
}

count_rows :: proc(conn: ^db.Sqlite3, sql: string) -> int {
	stmt: ^db.Sqlite3_Stmt
	n := -1

	if db.sqlite3_prepare_v3(conn, raw_data(sql), i32(len(sql)), 0, &stmt, nil) == 0 && db.sqlite3_step(stmt) == 100 {
		n = int(db.sqlite3_column_int64(stmt, 0))
	}

	db.sqlite3_finalize(stmt)
	return n
}

/* Migrate a fresh database, tamper with what it recorded, and expect the next migration refused. */
refused :: proc(label: string, tamper: cstring, expected: string) {
	conn := fresh()
	defer db.sqlite3_close_v2(conn)

	_, _, first := db.migrate(conn, "plan_test")
	check(first == .OK, "first migration")
	check(db.sqlite3_exec(conn, tamper, nil, nil, nil) == 0, "tamper")

	applied, message, rc := db.migrate(conn, "plan_test")
	defer delete(message)

	if rc == .OK || !strings.contains(message, expected) {
		fmt.printfln("plan_test.odin: %s: expected %q, got %v %q", label, expected, rc, message)
		failures += 1
	}

	check(applied == 0, "nothing applied")
	check(db.sqlite3_get_autocommit(conn) != 0, "no transaction left open")
}

migration :: proc() {
	conn := fresh()
	applied, message, rc := db.migrate(conn, "plan_test 1.0")

	check(rc == .OK && applied == 2 && message == "", "migrates")
	applied, message, rc = db.migrate(conn, "plan_test 1.0")
	check(rc == .OK && applied == 0, "second migration does nothing")
	check(count_rows(conn, "select count(*) from _sys_patch where applied_by = 'plan_test 1.0' and created_at > 0") == 2, "patches recorded")
	check(count_rows(conn, "select count(*) from _sys_script") == 2, "scripts recorded")
	db.sqlite3_close_v2(conn)

	refused("changed", "update _sys_script set sql = sql || ' ' where name = 'plan.sql'",
		"patch 1.0.0: plan.sql changed since it was applied")
	refused("renamed", "update _sys_script set name = 'other.sql' where name = 'role.sql'",
		"patch 1.1.0: other.sql was applied where this program carries role.sql")
	refused("not recorded", "delete from _sys_script where name = 'role.sql'",
		"patch 1.1.0: role.sql was added after the patch was applied")
	refused("extra", "insert into _sys_script (patch, ordinal, name, sql, created_at) select serial, 1, 'extra.sql', 'select 1', 0 from _sys_patch where minor = 1",
		"patch 1.1.0: extra.sql was applied, but this program does not carry it")
	refused("newer program", "insert into _sys_patch (major, minor, patch, prerelease, applied_by, created_at) values (9, 0, 0, 'rc.1', 'x', 0)",
		"the database has patch 9.0.0-rc.1, which this program does not carry")
	refused("out of order", "delete from _sys_patch where minor = 0",
		"patch 1.0.0 is older than 1.1.0, which is already applied")
}

queries :: proc() {
	conn := fresh()
	defer db.sqlite3_close_v2(conn)

	_, _, migrated := db.migrate(conn, "plan_test")
	check(migrated == .OK, "migrates")

	ctx := db.context_make(conn)
	defer db.context_destroy(&ctx)

	now: i64 = 1000
	ada, rc := db.users_insert(&ctx, "ada", "ada@x", nil, "admin", now, nil)
	check(rc == .OK, "insert ada")
	bob: db.Users_Serial
	bob, rc = db.users_insert(&ctx, "bob", "bob@x", "Bob", "admin", now, nil)
	check(rc == .OK, "insert bob")

	count: db.Count_Users_Row
	count, rc = db.count_users(&ctx)
	check(rc == .ROW && count.total == 2, "count_users")

	named: db.User_By_Name_Row
	named, rc = db.user_by_name(&ctx, "bob")
	text, has := named.display_text.?
	check(rc == .ROW && named.serial == bob && named.email == "bob@x" && has && text == "Bob", "user_by_name bob")
	db.user_by_name_row_destroy(&named)
	named, rc = db.user_by_name(&ctx, "ada")
	check(rc == .ROW && named.display_text == nil, "user_by_name ada")
	db.user_by_name_row_destroy(&named)
	named, rc = db.user_by_name(&ctx, "nobody")
	check(rc == .DONE, "user_by_name nobody")

	check(db.add_session(&ctx, ada, "f", "h1", now + 10, now) == .OK, "add h1")
	check(db.add_session(&ctx, ada, "f", "h2", now + 20, now) == .OK, "add h2")
	check(db.add_session(&ctx, bob, "f", "h3", now + 30, now) == .OK, "add h3")
	check(db.add_session(&ctx, ada, "f", "h1", now, now) == .CONSTRAINT, "duplicate token")
	check(db.touch_session(&ctx, now + 5, db.Sessions_Serial(1)) == .OK, "touch")
	check(db.touch_session(&ctx, now, db.Sessions_Serial(99)) == .DONE, "touch nothing")

	{
		sessions, opened := db.user_sessions(&ctx, ada, now + 6)
		defer db.cursor_close(&sessions)
		check(opened == .OK, "user_sessions")
		n := 0

		for {
			s, step := db.cursor_next(&sessions)
			if step != .ROW do break
			check(s.serial == db.Sessions_Serial(2) && s.expires_at == now + 20 && s.username == "ada", "session row")
			db.user_sessions_row_destroy(&s)
			n += 1
		}

		check(n == 1, "one session")
	}

	{
		/* A query selecting a table's columns reads that table's row. */
		users, opened := db.users_by_role(&ctx, "admin")
		defer db.cursor_close(&users)
		check(opened == .OK, "users_by_role")
		n := 0

		for {
			u, step := db.cursor_next(&users)
			if step != .ROW do break
			check(u.username == ("ada" if n == 0 else "bob"), "users by role order")
			db.users_row_destroy(&u)
			n += 1
		}

		check(n == 2, "two admins")
	}

	{
		found, opened := db.search_users(&ctx, "B")
		defer db.cursor_close(&found)
		check(opened == .OK, "search_users")
		f, step := db.cursor_next(&found)
		check(step == .ROW && f.serial == bob, "found bob")
		db.search_users_row_destroy(&f)
		_, step = db.cursor_next(&found)
		check(step == .DONE, "only bob")
	}

	{
		_, inserted := db.tag_insert(&ctx, nil, "news", nil)
		check(inserted == .OK, "tag insert")
		ranks, opened := db.tag_ranks(&ctx)
		defer db.cursor_close(&ranks)
		check(opened == .OK, "tag_ranks")
		r, step := db.cursor_next(&ranks)
		check(step == .ROW && r.slug == "news" && r.position == 1, "rank")
		db.tag_ranks_row_destroy(&r)
	}
}

main :: proc() {
	track: mem.Tracking_Allocator
	mem.tracking_allocator_init(&track, context.allocator)
	context.allocator = mem.tracking_allocator(&track)

	migration()

	check(db.systables != nil, "systables embedded")
	check(len(db.patches) == 2 && db.patches[1].version == "1.1.0" && db.patches[1].minor == 1, "patches embedded")
	check(len(db.schema_hash) == 16, "schema hash")

	conn := fresh()
	_, _, migrated := db.migrate(conn, "plan_test")
	check(migrated == .OK, "migrates")

	{
		ctx := db.context_make(conn)
		defer db.context_destroy(&ctx)

		/* users: a default, a nullable column left NULL, the autoincrement key returned typed. */
		now: i64 = 1000
		ada, rc := db.users_insert(&ctx, "ada", "ada@x", nil, nil, now, nil)
		check(rc == .OK, "insert ada")
		bob: db.Users_Serial
		bob, rc = db.users_insert(&ctx, "bob", "bob@x", "Bob", "admin", now, now)
		check(rc == .OK && u64(ada) == 1 && u64(bob) == 2, "keys")
		_, rc = db.users_insert(&ctx, "ada", "other", nil, nil, now, nil)
		check(rc == .CONSTRAINT, "duplicate username")

		row: db.Users_Row
		row, rc = db.users_get_by_key(&ctx, ada)
		check(rc == .ROW && row.serial == ada && row.username == "ada" && row.role == "reader", "get ada")
		check(row.display_text == nil && row.updated_at == nil && row.created_at == 1000, "nulls and values")
		db.users_row_destroy(&row)

		row, rc = db.users_get_by_username(&ctx, "bob")
		text, has := row.display_text.?
		updated, stamped := row.updated_at.?
		check(rc == .ROW && row.serial == bob && has && text == "Bob" && stamped && updated == now, "get bob")
		db.users_row_destroy(&row)
		_, rc = db.users_get_by_email(&ctx, "nobody")
		check(rc == .DONE, "no such email")

		check(db.users_update_by_key(&ctx, "bob", "bob@y", nil, "admin", now, nil, bob) == .OK, "update")
		check(db.users_update_by_key(&ctx, "x", "x", nil, "x", now, nil, db.Users_Serial(99)) == .DONE, "update nothing")
		check(db.users_upsert_on_username(&ctx, "bob", "bob@z", nil, nil, now, nil) == .OK, "upsert")
		row, rc = db.users_get_by_key(&ctx, bob)
		check(rc == .ROW && row.email == "bob@z" && row.role == "reader" && row.display_text == nil, "after upsert")
		db.users_row_destroy(&row)

		/* sessions: a list, and the same list nested inside itself. */
		families := [3]string{"f1", "f1", "f2"}

		for family, i in families {
			hash := fmt.tprintf("h%d", i)
			_, inserted := db.sessions_insert(&ctx, ada, family, hash, now + i64(i), now)
			check(inserted == .OK, "session insert")
		}

		pairs := 0

		{
			outer, opened := db.sessions_list_by_user(&ctx, ada)
			defer db.cursor_close(&outer)
			check(opened == .OK, "outer list")

			for {
				s, step := db.cursor_next(&outer)
				if step != .ROW do break
				check(s.user == ada, "session user")
				db.sessions_row_destroy(&s)

				inner, again := db.sessions_list_by_user(&ctx, ada)
				check(again == .OK, "inner list")

				for {
					t, inner_step := db.cursor_next(&inner)
					if inner_step != .ROW do break
					db.sessions_row_destroy(&t)
					pairs += 1
				}

				db.cursor_close(&inner)
			}
		}

		check(pairs == 9, "nested cursors")

		{
			/* A cursor keeps what it was bound with, though the argument changes before it steps. */
			family := strings.clone("f1")
			outer, opened := db.sessions_list_by_user_and_family(&ctx, ada, family)
			defer db.cursor_close(&outer)
			check(opened == .OK, "list by family")
			raw_data(family)[0] = 'z'
			raw_data(family)[1] = 'z'
			n := 0

			for {
				s, step := db.cursor_next(&outer)
				if step != .ROW do break
				check(s.family == "f1", "family")
				db.sessions_row_destroy(&s)
				n += 1
			}

			delete(family)
			check(n == 2, "cursor kept its argument")
		}

		s: db.Sessions_Row
		s, rc = db.sessions_get_by_token_hash(&ctx, "h2")
		check(rc == .ROW && s.expires_at == now + 2, "by token")
		db.sessions_row_destroy(&s)
		check(db.sessions_delete_by_user_and_family(&ctx, ada, "f1") == .OK, "delete")
		check(db.sessions_delete_by_user_and_family(&ctx, ada, "f1") == .DONE, "delete nothing")

		/* tag: an optional key and a default, both left out; then both given. */
		first, second: db.Tag_Id
		first, rc = db.tag_insert(&ctx, nil, "news", nil)
		check(rc == .OK && u64(first) == 1, "tag defaults")
		second, rc = db.tag_insert(&ctx, db.Tag_Id(40), "art", 5)
		check(rc == .OK && u64(second) == 40, "tag given")

		tag: db.Tag_Row
		tag, rc = db.tag_get_by_key(&ctx, first)
		check(rc == .ROW && tag.order == 0, "default order")
		db.tag_row_destroy(&tag)
		tag, rc = db.tag_get_by_slug(&ctx, "art")
		check(rc == .ROW && tag.order == 5, "given order")
		db.tag_row_destroy(&tag)

		/* user_tag: a junction, linked and unlinked. */
		check(db.user_tag_link(&ctx, ada, first) == .OK, "link")
		check(db.user_tag_link(&ctx, ada, first) == .DONE, "link again")
		check(db.user_tag_link(&ctx, db.Users_Serial(77), first) == .CONSTRAINT, "link to nobody")
		check(db.user_tag_unlink(&ctx, ada, first) == .OK, "unlink")
		check(db.user_tag_unlink(&ctx, ada, first) == .DONE, "unlink again")

		/* setting: a text key of a WITHOUT ROWID table. */
		check(db.setting_insert(&ctx, "theme", "dark") == .OK, "setting insert")
		check(db.setting_upsert_on_key(&ctx, "theme", "light") == .OK, "setting upsert")
		setting: db.Setting_Row
		setting, rc = db.setting_get_by_key(&ctx, "theme")
		check(rc == .ROW && setting.value == "light", "setting value")
		db.setting_row_destroy(&setting)
		check(db.setting_delete_by_key(&ctx, "theme") == .OK, "setting delete")
		_, rc = db.setting_get_by_key(&ctx, "theme")
		check(rc == .DONE, "setting gone")

		/* doubled: a generated column is read but never written. */
		key: db.Doubled_Id
		key, rc = db.doubled_insert(&ctx, nil, 21)
		check(rc == .OK, "doubled insert")
		d: db.Doubled_Row
		d, rc = db.doubled_get_by_key(&ctx, key)
		twice, computed := d.twice.?
		check(rc == .ROW && d.w == 21 && computed && twice == 42, "generated column")

		/* A statement selecting other columns is refused by the reader. */
		stmt: ^db.Sqlite3_Stmt
		sql := "select username, serial, email, display_text, role, created_at, updated_at from users"
		check(db.sqlite3_prepare_v3(conn, raw_data(sql), i32(len(sql)), 0, &stmt, nil) == 0 && db.sqlite3_step(stmt) == 100, "prepare")
		_, rc = db.users_read(stmt, context.allocator)
		check(rc == .MISMATCH, "mismatch")
		db.sqlite3_finalize(stmt)

		/* An id SQLite cannot hold is refused before it is bound. */
		_, rc = db.users_get_by_key(&ctx, db.Users_Serial(max(u64)))
		check(rc == .RANGE, "range")

		/* A cursor that was never opened. */
		unopened: db.Cursor(db.Users_Row)
		_, rc = db.cursor_next(&unopened)
		check(rc == .MISUSE, "misuse")

		_, rc = db.sys_state_get_by_key(&ctx, db.Sys_State_Id(1))
		check(rc == .DONE, "sys_state")
	}

	db.sqlite3_close_v2(conn)
	queries()

	for _, leak in track.allocation_map {
		fmt.printfln("plan_test.odin: %v bytes allocated at %v were never freed", leak.size, leak.location)
		failures += 1
	}

	for bad in track.bad_free_array {
		fmt.printfln("plan_test.odin: %p freed at %v was not allocated", bad.memory, bad.location)
		failures += 1
	}

	fmt.printfln("plan_test.odin: %s", "FAILED" if failures > 0 else "ok")
	os.exit(1 if failures > 0 else 0)
}
