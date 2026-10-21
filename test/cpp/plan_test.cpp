/* Drives the C++ generated for test/cases/plan, its query files included, against a real database:
 * every kind of statement and query, the return codes, nullable values, cursors, typed ids, the
 * embedded scripts and migration. */

#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_set>

#include "db.hpp"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("plan_test.cpp:%d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static sqlite3 *fresh() {
	sqlite3 *db = nullptr;

	CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
	CHECK(sqlite3_exec(db, "pragma foreign_keys = on", nullptr, nullptr, nullptr) == SQLITE_OK);
	return db;
}

static int count_rows(sqlite3 *db, const char *sql) {
	sqlite3_stmt *stmt = nullptr;
	int           n    = -1;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
		n = sqlite3_column_int(stmt, 0);
	}

	sqlite3_finalize(stmt);
	return n;
}

/* Migrate a fresh database, tamper with what it recorded, and expect the next migration refused. */
static void refused(const char *label, const char *tamper, const char *expected) {
	sqlite3    *db      = fresh();
	std::string message;
	size_t      applied = 99;

	CHECK(db::migrate(db, "plan_test", nullptr, nullptr) == SQLITE_OK);
	CHECK(sqlite3_exec(db, tamper, nullptr, nullptr, nullptr) == SQLITE_OK);

	int rc = db::migrate(db, "plan_test", &applied, &message);

	if (rc == SQLITE_OK || message.find(expected) == std::string::npos) {
		printf("plan_test.cpp: %s: expected \"%s\", got %d \"%s\"\n", label, expected, rc, message.c_str());
		++failures;
	}

	CHECK(applied == 0);
	CHECK(sqlite3_get_autocommit(db));
	sqlite3_close(db);
}

static void migration() {
	sqlite3    *db      = fresh();
	size_t      applied = 0;
	std::string message;

	CHECK(db::migrate(db, "plan_test 1.0", &applied, &message) == SQLITE_OK && applied == 2 && message.empty());
	CHECK(db::migrate(db, "plan_test 1.0", &applied, &message) == SQLITE_OK && applied == 0);
	CHECK(count_rows(db, "select count(*) from _sys_patch where applied_by = 'plan_test 1.0' and created_at > 0") == 2);
	CHECK(count_rows(db, "select count(*) from _sys_script") == 2);
	sqlite3_close(db);

	refused("changed", "update _sys_script set sql = sql || ' ' where name = 'plan.sql'",
			"patch 1.0.0: plan.sql changed since it was applied");
	refused("renamed", "update _sys_script set name = 'other.sql' where name = 'role.sql'",
			"patch 1.1.0: other.sql was applied where this program carries role.sql");
	refused("not recorded", "delete from _sys_script where name = 'role.sql'",
			"patch 1.1.0: role.sql was added after the patch was applied");
	refused("extra", "insert into _sys_script (patch, ordinal, name, sql, created_at)"
			" select serial, 1, 'extra.sql', 'select 1', 0 from _sys_patch where minor = 1",
			"patch 1.1.0: extra.sql was applied, but this program does not carry it");
	refused("newer program", "insert into _sys_patch (major, minor, patch, prerelease, applied_by, created_at)"
			" values (9, 0, 0, 'rc.1', 'x', 0)",
			"the database has patch 9.0.0-rc.1, which this program does not carry");
	refused("out of order", "delete from _sys_patch where minor = 0",
			"patch 1.0.0 is older than 1.1.0, which is already applied");
}

/* Typed ids: their own types, compared and hashed by value, cast explicitly. */
static void ids() {
	db::UsersSerial a = { 3 };
	db::UsersSerial b = { 4 };

	CHECK(a != b && a < b && b > a && a <= a && b >= a && a == db::UsersSerial{ 3 });
	CHECK(static_cast<uint64_t>(a) == 3);

	std::unordered_set<db::UsersSerial> seen;

	seen.insert(a);
	seen.insert(db::UsersSerial{ 3 });
	CHECK(seen.size() == 1);
}

static void queries() {
	sqlite3 *db = fresh();

	CHECK(db::migrate(db, "plan_test", nullptr, nullptr) == SQLITE_OK);

	db::Context     ctx(db);
	db::UsersSerial ada = { 0 };
	db::UsersSerial bob = { 0 };
	int64_t         now = 1000;

	CHECK(db::users::insert(ctx, "ada", "ada@x", {}, db::some("admin"), now, {}, &ada) == SQLITE_OK);
	CHECK(db::users::insert(ctx, "bob", "bob@x", db::some("Bob"), db::some("admin"), now, {}, &bob) == SQLITE_OK);

	db::count_users::Row count = db::count_users::Row();

	CHECK(db::count_users::run(ctx, count) == SQLITE_ROW && count.total == 2);

	db::user_by_name::Row named = db::user_by_name::Row();

	CHECK(db::user_by_name::run(ctx, "bob", named) == SQLITE_ROW);
	CHECK(named.serial == bob && named.email == "bob@x" && named.display_text && *named.display_text == "Bob");
	CHECK(db::user_by_name::run(ctx, "ada", named) == SQLITE_ROW && !named.display_text);
	CHECK(db::user_by_name::run(ctx, "nobody", named) == SQLITE_DONE);

	CHECK(db::add_session::run(ctx, ada, "f", "h1", now + 10, now) == SQLITE_OK);
	CHECK(db::add_session::run(ctx, ada, "f", "h2", now + 20, now) == SQLITE_OK);
	CHECK(db::add_session::run(ctx, bob, "f", "h3", now + 30, now) == SQLITE_OK);
	CHECK(db::add_session::run(ctx, ada, "f", "h1", now, now) == SQLITE_CONSTRAINT);
	CHECK(db::touch_session::run(ctx, now + 5, db::SessionsSerial{ 1 }) == SQLITE_OK);
	CHECK(db::touch_session::run(ctx, now, db::SessionsSerial{ 99 }) == SQLITE_DONE);

	db::Cursor<db::user_sessions::Row> sessions;
	db::user_sessions::Row             s = db::user_sessions::Row();
	int                                n = 0;

	CHECK(db::user_sessions::run(ctx, ada, now + 6, sessions) == SQLITE_OK);

	while (sessions.next(s) == SQLITE_ROW) {
		CHECK(s.serial.v == 2 && s.expires_at == now + 20 && s.username == "ada");
		++n;
	}

	CHECK(n == 1);

	/* A query selecting a table's columns uses that table's row. */
	db::Cursor<db::users::Row> users;
	db::users::Row             u = db::users::Row();

	n = 0;
	CHECK(db::users_by_role::run(ctx, "admin", users) == SQLITE_OK);

	while (users.next(u) == SQLITE_ROW) {
		CHECK(u.username == (n == 0 ? "ada" : "bob"));
		++n;
	}

	CHECK(n == 2);

	db::Cursor<db::search_users::Row> found;
	db::search_users::Row             f = db::search_users::Row();

	CHECK(db::search_users::run(ctx, "B", found) == SQLITE_OK);
	CHECK(found.next(f) == SQLITE_ROW && f.serial == bob);
	CHECK(found.next(f) == SQLITE_DONE);

	db::TagId                      tag = { 0 };
	db::Cursor<db::tag_ranks::Row> ranks;
	db::tag_ranks::Row             r = db::tag_ranks::Row();

	CHECK(db::tag::insert(ctx, {}, "news", {}, &tag) == SQLITE_OK);
	CHECK(db::tag_ranks::run(ctx, ranks) == SQLITE_OK);
	CHECK(ranks.next(r) == SQLITE_ROW && r.slug == "news" && r.position == 1);
	ranks.close();
	sessions.close();
	users.close();
	found.close();
	sqlite3_close_v2(db);
}

int main() {
	migration();
	ids();

	sqlite3 *db = fresh();

	CHECK(db::systables != nullptr);
	CHECK(db::patch_count == 2 && strcmp(db::patches[1].version, "1.1.0") == 0 && db::patches[1].minor == 1);
	CHECK(strlen(db::schema_hash) == 16);
	CHECK(db::migrate(db, "plan_test", nullptr, nullptr) == SQLITE_OK);

	{
		db::Context ctx(db);

		/* users: a default, a nullable column left NULL, the autoincrement key returned typed. */
		db::UsersSerial ada = { 0 };
		db::UsersSerial bob = { 0 };
		int64_t         now = 1000;

		CHECK(db::users::insert(ctx, "ada", "ada@x", {}, {}, now, {}, &ada) == SQLITE_OK);
		CHECK(db::users::insert(ctx, "bob", "bob@x", db::some("Bob"), db::some("admin"), now, db::some(now), &bob) == SQLITE_OK);
		CHECK(ada.v == 1 && bob.v == 2);
		CHECK(db::users::insert(ctx, "ada", "other", {}, {}, now, {}, nullptr) == SQLITE_CONSTRAINT);

		db::users::Row row = db::users::Row();

		CHECK(db::users::get_by_key(ctx, ada, row) == SQLITE_ROW);
		CHECK(row.serial == ada && row.username == "ada" && row.role == "reader");
		CHECK(!row.display_text && !row.updated_at && row.created_at == 1000);
		CHECK(row.display_text.value_or("none") == "none");

		/* The same row is reused: everything is replaced. */
		CHECK(db::users::get_by_username(ctx, "bob", row) == SQLITE_ROW);
		CHECK(row.serial == bob && row.display_text && *row.display_text == "Bob" && row.updated_at.value_or(0) == now);
		CHECK(db::users::get_by_email(ctx, "nobody", row) == SQLITE_DONE);

		CHECK(db::users::update_by_key(ctx, "bob", "bob@y", {}, "admin", now, {}, bob) == SQLITE_OK);
		CHECK(db::users::update_by_key(ctx, "x", "x", {}, "x", now, {}, db::UsersSerial{ 99 }) == SQLITE_DONE);
		CHECK(db::users::upsert_on_username(ctx, "bob", "bob@z", {}, {}, now, {}) == SQLITE_OK);
		CHECK(db::users::get_by_key(ctx, bob, row) == SQLITE_ROW && row.email == "bob@z" && row.role == "reader");
		CHECK(!row.display_text);

		/* sessions: a list, and the same list nested inside itself. */
		const char *families[] = { "f1", "f1", "f2" };

		for (int i = 0; i < 3; ++i) {
			CHECK(db::sessions::insert(ctx, ada, families[i], "h" + std::to_string(i), now + i, now, nullptr) == SQLITE_OK);
		}

		db::sessions::Row s = db::sessions::Row();
		db::sessions::Row t = db::sessions::Row();
		int               pairs = 0;

		{
			db::Cursor<db::sessions::Row> outer;

			CHECK(db::sessions::list_by_user(ctx, ada, outer) == SQLITE_OK);

			while (outer.next(s) == SQLITE_ROW) {
				db::Cursor<db::sessions::Row> inner;

				CHECK(s.user == ada);
				CHECK(db::sessions::list_by_user(ctx, ada, inner) == SQLITE_OK);

				while (inner.next(t) == SQLITE_ROW) {
					++pairs;
				}
			}
		}

		CHECK(pairs == 9);

		int count = 0;

		{
			db::Cursor<db::sessions::Row> outer;

			CHECK(db::sessions::list_by_user_and_family(ctx, ada, "f1", outer) == SQLITE_OK);

			while (outer.next(s) == SQLITE_ROW) {
				CHECK(s.family == "f1");
				++count;
			}
		}

		CHECK(count == 2);

		/* A cursor keeps what it was bound with, though the argument changes or dies before it steps. */
		{
			db::Cursor<db::sessions::Row> outer;
			std::string                   family = "f1";

			CHECK(db::sessions::list_by_user_and_family(ctx, ada, family, outer) == SQLITE_OK);
			family.assign("zz");
			count = 0;

			while (outer.next(s) == SQLITE_ROW) {
				++count;
			}
		}

		CHECK(count == 2);
		CHECK(db::sessions::get_by_token_hash(ctx, "h2", s) == SQLITE_ROW && s.expires_at == now + 2);
		CHECK(db::sessions::delete_by_user_and_family(ctx, ada, "f1") == SQLITE_OK);
		CHECK(db::sessions::delete_by_user_and_family(ctx, ada, "f1") == SQLITE_DONE);

		/* tag: an optional key and a default, both left out; then both given. */
		db::TagId first  = { 0 };
		db::TagId second = { 0 };

		CHECK(db::tag::insert(ctx, {}, "news", {}, &first) == SQLITE_OK && first.v == 1);
		CHECK(db::tag::insert(ctx, db::some(db::TagId{ 40 }), "art", db::some(int64_t(5)), &second) == SQLITE_OK && second.v == 40);

		db::tag::Row tag = db::tag::Row();

		CHECK(db::tag::get_by_key(ctx, first, tag) == SQLITE_ROW && tag.order == 0);
		CHECK(db::tag::get_by_slug(ctx, "art", tag) == SQLITE_ROW && tag.order == 5);

		/* user_tag: a junction, linked and unlinked. */
		CHECK(db::user_tag::link(ctx, ada, first) == SQLITE_OK);
		CHECK(db::user_tag::link(ctx, ada, first) == SQLITE_DONE);
		CHECK(db::user_tag::link(ctx, db::UsersSerial{ 77 }, first) == SQLITE_CONSTRAINT);
		CHECK(db::user_tag::unlink(ctx, ada, first) == SQLITE_OK);
		CHECK(db::user_tag::unlink(ctx, ada, first) == SQLITE_DONE);

		/* setting: a text key of a WITHOUT ROWID table. */
		db::setting::Row setting = db::setting::Row();

		CHECK(db::setting::insert(ctx, "theme", "dark") == SQLITE_OK);
		CHECK(db::setting::upsert_on_key(ctx, "theme", "light") == SQLITE_OK);
		CHECK(db::setting::get_by_key(ctx, "theme", setting) == SQLITE_ROW && setting.value == "light");
		CHECK(db::setting::delete_by_key(ctx, "theme") == SQLITE_OK);
		CHECK(db::setting::get_by_key(ctx, "theme", setting) == SQLITE_DONE);

		/* doubled: a generated column is read but never written. */
		db::doubled::Row d   = db::doubled::Row();
		db::DoubledId    key = { 0 };

		CHECK(db::doubled::insert(ctx, {}, 21, &key) == SQLITE_OK);
		CHECK(db::doubled::get_by_key(ctx, key, d) == SQLITE_ROW && d.w == 21 && d.twice && *d.twice == 42);

		/* A statement selecting other columns is refused by the reader. */
		sqlite3_stmt *stmt = nullptr;

		CHECK(sqlite3_prepare_v2(db, "select username, serial, email, display_text, role, created_at, updated_at"
				" from users", -1, &stmt, nullptr) == SQLITE_OK);
		CHECK(sqlite3_step(stmt) == SQLITE_ROW && db::users::read(stmt, row) == SQLITE_MISMATCH);
		sqlite3_finalize(stmt);

		/* An id SQLite cannot hold is refused before it is bound. */
		CHECK(db::users::get_by_key(ctx, db::UsersSerial{ UINT64_MAX }, row) == SQLITE_RANGE);

		/* A cursor that was never opened. */
		db::Cursor<db::users::Row> unopened;

		CHECK(unopened.next(row) == SQLITE_MISUSE);

		db::sys_state::Row state = db::sys_state::Row();

		CHECK(db::sys_state::get_by_key(ctx, db::SysStateId{ 1 }, state) == SQLITE_DONE);
	}

	sqlite3_close(db);
	queries();
	printf("plan_test.cpp: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
