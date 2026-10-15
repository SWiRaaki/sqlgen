/* Drives the C generated for test/cases/plan, its query files included, against a real database:
 * every kind of statement and query, the return codes, row ownership, cursors, typed ids, the
 * embedded scripts and migration. */

#include <stdio.h>
#include <string.h>

#include "db.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("plan_test:%d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static sqlite3 *fresh(void) {
	sqlite3 *db = NULL;

	CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
	CHECK(sqlite3_exec(db, "pragma foreign_keys = on", NULL, NULL, NULL) == SQLITE_OK);
	return db;
}

static int count_rows(sqlite3 *db, const char *sql) {
	sqlite3_stmt *stmt = NULL;
	int           n    = -1;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
		n = sqlite3_column_int(stmt, 0);
	}

	sqlite3_finalize(stmt);
	return n;
}

/* Migrate a fresh database, tamper with what it recorded, and expect the next migration refused. */
static void refused(const char *label, const char *tamper, const char *expected) {
	sqlite3 *db      = fresh();
	char    *message = NULL;
	size_t   applied = 99;

	CHECK(db_migrate(db, "plan_test", NULL, NULL) == SQLITE_OK);
	CHECK(sqlite3_exec(db, tamper, NULL, NULL, NULL) == SQLITE_OK);

	int rc = db_migrate(db, "plan_test", &applied, &message);

	if (rc == SQLITE_OK || !message || !strstr(message, expected)) {
		printf("plan_test: %s: expected \"%s\", got %d \"%s\"\n", label, expected, rc, message ? message : "");
		++failures;
	}

	CHECK(applied == 0);
	CHECK(sqlite3_get_autocommit(db));
	sqlite3_free(message);
	sqlite3_close(db);
}

static void migration(void) {
	sqlite3 *db      = fresh();
	size_t   applied = 0;
	char    *message = NULL;

	CHECK(db_migrate(db, "plan_test 1.0", &applied, &message) == SQLITE_OK && applied == 2 && !message);
	CHECK(db_migrate(db, "plan_test 1.0", &applied, &message) == SQLITE_OK && applied == 0);
	CHECK(count_rows(db, "select count(*) from _sys_patch where applied_by = 'plan_test 1.0' and created_at > 0") == 2);
	CHECK(count_rows(db, "select count(*) from _sys_script") == 2);

	sqlite3_stmt *stmt = NULL;

	CHECK(sqlite3_prepare_v2(db, "select sql from _sys_script where name = 'plan.sql'", -1, &stmt, NULL) == SQLITE_OK);
	CHECK(sqlite3_step(stmt) == SQLITE_ROW && strcmp((const char *)sqlite3_column_text(stmt, 0), db_patches[0].scripts[0].sql) == 0);
	sqlite3_finalize(stmt);

	/* Build metadata does not tell patches apart. */
	CHECK(sqlite3_exec(db, "update _sys_patch set build = 'other'", NULL, NULL, NULL) == SQLITE_OK);
	CHECK(db_migrate(db, "plan_test 1.0", &applied, NULL) == SQLITE_OK && applied == 0);
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

static int text_is(const db_text *text, const char *expected) {
	return text->ptr && text->len == strlen(expected) && strcmp(text->ptr, expected) == 0;
}

/* The query files: each kind of result, positional and named parameters, a reused row type. */
static void queries(void) {
	sqlite3 *db  = fresh();
	db_ctx  *ctx = NULL;

	CHECK(db_migrate(db, "plan_test", NULL, NULL) == SQLITE_OK);
	CHECK(db_ctx_open(&ctx, db) == SQLITE_OK);

	db_users_serial ada = DB_USERS_SERIAL(0);
	db_users_serial bob = DB_USERS_SERIAL(0);
	int64_t         now = 1000;

	CHECK(db_users_insert(ctx, "ada", 3, "ada@x", 5, NULL, 0, "admin", 5, now, NULL, &ada) == SQLITE_OK);
	CHECK(db_users_insert(ctx, "bob", 3, "bob@x", 5, "Bob", 3, "admin", 5, now, NULL, &bob) == SQLITE_OK);

	/* Exactly one row, whatever the table holds. */
	db_count_users_row count = {0};

	CHECK(db_count_users(ctx, &count) == SQLITE_ROW && count.total == 2);
	db_count_users_row_free(&count);

	/* At most one row, by a positional parameter named after its column. */
	db_user_by_name_row named = {0};

	CHECK(db_user_by_name(ctx, "bob", 3, &named) == SQLITE_ROW);
	CHECK(named.serial.v == bob.v && text_is(&named.email, "bob@x") && named.has_display_text);
	CHECK(text_is(&named.display_text, "Bob"));
	CHECK(db_user_by_name(ctx, "ada", 3, &named) == SQLITE_ROW && !named.has_display_text);
	CHECK(db_user_by_name(ctx, "nobody", 6, &named) == SQLITE_DONE);
	db_user_by_name_row_free(&named);

	/* Writes: an insert with positional parameters, an update by changed rows. */
	CHECK(db_add_session(ctx, ada, "f", 1, "h1", 2, now + 10, now) == SQLITE_OK);
	CHECK(db_add_session(ctx, ada, "f", 1, "h2", 2, now + 20, now) == SQLITE_OK);
	CHECK(db_add_session(ctx, bob, "f", 1, "h3", 2, now + 30, now) == SQLITE_OK);
	CHECK(db_add_session(ctx, ada, "f", 1, "h1", 2, now, now) == SQLITE_CONSTRAINT);
	CHECK(db_touch_session(ctx, now + 5, DB_SESSIONS_SERIAL(1)) == SQLITE_OK);
	CHECK(db_touch_session(ctx, now, DB_SESSIONS_SERIAL(99)) == SQLITE_DONE);

	/* Many rows of a row type of its own, joined. */
	db_cursor              cursor = {0};
	db_user_sessions_row   s      = {0};
	int                    n      = 0;

	CHECK(db_user_sessions(ctx, ada, now + 6, &cursor) == SQLITE_OK);

	while (db_user_sessions_next(&cursor, &s) == SQLITE_ROW) {
		CHECK(s.serial.v == 2 && s.expires_at == now + 20 && text_is(&s.username, "ada"));
		++n;
	}

	CHECK(n == 1);
	db_cursor_close(&cursor);
	db_user_sessions_row_free(&s);

	/* Many rows of the table's own row type. */
	db_users_row u = {0};

	n = 0;
	CHECK(db_users_by_role(ctx, "admin", 5, &cursor) == SQLITE_OK);

	while (db_users_next(&cursor, &u) == SQLITE_ROW) {
		CHECK(text_is(&u.username, n == 0 ? "ada" : "bob"));
		++n;
	}

	CHECK(n == 2);
	db_cursor_close(&cursor);
	db_users_row_free(&u);

	/* Types from annotations. */
	db_search_users_row found = {0};
	db_tag_ranks_row    rank  = {0};
	db_tag_id           tag   = DB_TAG_ID(0);

	CHECK(db_search_users(ctx, "B", 1, &cursor) == SQLITE_OK);
	CHECK(db_search_users_next(&cursor, &found) == SQLITE_ROW && found.serial.v == bob.v);
	CHECK(db_search_users_next(&cursor, &found) == SQLITE_DONE);
	db_cursor_close(&cursor);
	db_search_users_row_free(&found);

	CHECK(db_tag_insert(ctx, NULL, "news", 4, NULL, &tag) == SQLITE_OK);
	CHECK(db_tag_ranks(ctx, &cursor) == SQLITE_OK);
	CHECK(db_tag_ranks_next(&cursor, &rank) == SQLITE_ROW && text_is(&rank.slug, "news") && rank.position == 1);
	db_cursor_close(&cursor);
	db_tag_ranks_row_free(&rank);

	db_ctx_close(ctx);
	sqlite3_close(db);
}

int main(void) {
	sqlite3 *db  = NULL;
	db_ctx  *ctx = NULL;

	migration();

	/* The schema, from the scripts the generator embedded. */
	db = fresh();
	CHECK(db_systables != NULL);
	CHECK(db_patch_count == 2 && strcmp(db_patches[1].version, "1.1.0") == 0 && db_patches[1].minor == 1);
	CHECK(db_migrate(db, "plan_test", NULL, NULL) == SQLITE_OK);

	CHECK(strlen(db_schema_hash) == 16);
	CHECK(db_ctx_open(&ctx, db) == SQLITE_OK);

	/* users: a default, a nullable column left NULL, the autoincrement key returned typed. */
	db_users_serial ada = DB_USERS_SERIAL(0);
	db_users_serial bob = DB_USERS_SERIAL(0);
	int64_t         now = 1000;

	CHECK(db_users_insert(ctx, "ada", 3, "ada@x", 5, NULL, 0, NULL, 0, now, NULL, &ada) == SQLITE_OK);
	CHECK(db_users_insert(ctx, "bob", 3, "bob@x", 5, "Bob", 3, "admin", 5, now, &now, &bob) == SQLITE_OK);
	CHECK(ada.v == 1 && bob.v == 2);
	CHECK(db_users_insert(ctx, "ada", 3, "other", 5, NULL, 0, NULL, 0, now, NULL, NULL) == SQLITE_CONSTRAINT);

	db_users_row row = {0};

	CHECK(db_users_get_by_key(ctx, ada, &row) == SQLITE_ROW);
	CHECK(row.serial.v == 1 && text_is(&row.username, "ada") && text_is(&row.role, "reader"));
	CHECK(!row.has_display_text && row.display_text.ptr == NULL && !row.has_updated_at);
	CHECK(row.created_at == 1000);

	/* The same row object is reused: what it held is freed first. */
	CHECK(db_users_get_by_username(ctx, "bob", 3, &row) == SQLITE_ROW);
	CHECK(row.serial.v == 2 && text_is(&row.display_text, "Bob") && row.has_display_text && row.has_updated_at);
	CHECK(db_users_get_by_email(ctx, "nobody", 6, &row) == SQLITE_DONE);

	/* update and upsert. */
	CHECK(db_users_update_by_key(ctx, "bob", 3, "bob@y", 5, NULL, 0, "admin", 5, now, NULL, bob) == SQLITE_OK);
	CHECK(db_users_update_by_key(ctx, "x", 1, "x", 1, NULL, 0, "x", 1, now, NULL, DB_USERS_SERIAL(99)) == SQLITE_DONE);
	CHECK(db_users_upsert_on_username(ctx, "bob", 3, "bob@z", 5, NULL, 0, NULL, 0, now, NULL) == SQLITE_OK);
	CHECK(db_users_get_by_key(ctx, bob, &row) == SQLITE_ROW && text_is(&row.email, "bob@z") && text_is(&row.role, "reader"));
	db_users_row_free(&row);

	/* sessions: a list, and the same list nested inside itself. */
	const char *families[] = { "f1", "f1", "f2" };

	for (int i = 0; i < 3; ++i) {
		char hash[16];

		snprintf(hash, sizeof hash, "h%d", i);
		CHECK(db_sessions_insert(ctx, ada, families[i], 2, hash, strlen(hash), now + i, now, NULL) == SQLITE_OK);
	}

	db_cursor      outer = {0};
	db_cursor      inner = {0};
	db_sessions_row s    = {0};
	db_sessions_row t    = {0};
	int             pairs = 0;

	CHECK(db_sessions_list_by_user(ctx, ada, &outer) == SQLITE_OK);

	while (db_sessions_next(&outer, &s) == SQLITE_ROW) {
		CHECK(s.user.v == ada.v);
		CHECK(db_sessions_list_by_user(ctx, ada, &inner) == SQLITE_OK);
		CHECK(inner.owned);

		while (db_sessions_next(&inner, &t) == SQLITE_ROW) {
			++pairs;
		}

		db_cursor_close(&inner);
	}

	db_cursor_close(&outer);
	CHECK(pairs == 9);

	int count = 0;

	CHECK(db_sessions_list_by_user_and_family(ctx, ada, "f1", 2, &outer) == SQLITE_OK);
	CHECK(!outer.owned);

	while (db_sessions_next(&outer, &s) == SQLITE_ROW) {
		CHECK(text_is(&s.family, "f1"));
		++count;
	}

	db_cursor_close(&outer);
	CHECK(count == 2);
	CHECK(db_sessions_get_by_token_hash(ctx, "h2", 2, &s) == SQLITE_ROW && s.expires_at == now + 2);
	CHECK(db_sessions_delete_by_user_and_family(ctx, ada, "f1", 2) == SQLITE_OK);
	CHECK(db_sessions_delete_by_user_and_family(ctx, ada, "f1", 2) == SQLITE_DONE);
	db_sessions_row_free(&s);
	db_sessions_row_free(&t);

	/* tag: an optional key and a default, both left out; then both given. */
	db_tag_id first  = DB_TAG_ID(0);
	db_tag_id second = DB_TAG_ID(0);
	int64_t   order  = 5;
	db_tag_id wanted = DB_TAG_ID(40);

	CHECK(db_tag_insert(ctx, NULL, "news", 4, NULL, &first) == SQLITE_OK && first.v == 1);
	CHECK(db_tag_insert(ctx, &wanted, "art", 3, &order, &second) == SQLITE_OK && second.v == 40);

	db_tag_row tag = {0};

	CHECK(db_tag_get_by_slug(ctx, "news", 4, &tag) == SQLITE_ROW && tag.order == 0);
	CHECK(db_tag_get_by_key(ctx, second, &tag) == SQLITE_ROW && tag.order == 5);
	db_tag_row_free(&tag);

	/* user_tag: a junction without rowid. */
	CHECK(db_user_tag_link(ctx, ada, first) == SQLITE_OK);
	CHECK(db_user_tag_link(ctx, ada, first) == SQLITE_DONE);
	CHECK(db_user_tag_link(ctx, bob, first) == SQLITE_OK);
	CHECK(db_user_tag_link(ctx, DB_USERS_SERIAL(77), first) == SQLITE_CONSTRAINT);

	db_user_tag_row link = {0};

	count = 0;
	CHECK(db_user_tag_list_by_tag(ctx, first, &outer) == SQLITE_OK);

	while (db_user_tag_next(&outer, &link) == SQLITE_ROW) {
		CHECK(link.tag.v == first.v && link.user.v == (uint64_t)count + 1);
		++count;
	}

	db_cursor_close(&outer);
	CHECK(count == 2);
	CHECK(db_user_tag_unlink(ctx, ada, first) == SQLITE_OK);
	CHECK(db_user_tag_get_by_key(ctx, ada, first, &link) == SQLITE_DONE);
	db_user_tag_row_free(&link);

	/* setting: WITHOUT ROWID, so its insert hands back no key. */
	db_setting_row setting = {0};

	CHECK(db_setting_insert(ctx, "theme", 5, "dark", 4) == SQLITE_OK);
	CHECK(db_setting_upsert_on_key(ctx, "theme", 5, "light", 5) == SQLITE_OK);
	CHECK(db_setting_get_by_key(ctx, "theme", 5, &setting) == SQLITE_ROW && text_is(&setting.value, "light"));
	CHECK(db_setting_update_by_key(ctx, "pale", 4, "theme", 5) == SQLITE_OK);
	CHECK(db_setting_delete_by_key(ctx, "theme", 5) == SQLITE_OK);
	CHECK(db_setting_get_by_key(ctx, "theme", 5, &setting) == SQLITE_DONE);
	db_setting_row_free(&setting);

	/* doubled: a generated column is read but never written. */
	db_doubled_row d = {0};
	db_doubled_id  key = DB_DOUBLED_ID(0);

	CHECK(db_doubled_insert(ctx, NULL, 21, &key) == SQLITE_OK);
	CHECK(db_doubled_get_by_key(ctx, key, &d) == SQLITE_ROW && d.w == 21 && d.twice == 42 && d.has_twice);
	db_doubled_row_free(&d);

	/* A statement selecting other columns is refused by the reader. */
	sqlite3_stmt *stmt = NULL;

	CHECK(sqlite3_prepare_v2(db, "select username, serial, email, display_text, role, created_at, updated_at"
			" from users", -1, &stmt, NULL) == SQLITE_OK);
	CHECK(sqlite3_step(stmt) == SQLITE_ROW && db_users_read(stmt, &row) == SQLITE_MISMATCH);
	sqlite3_finalize(stmt);

	/* An id SQLite cannot hold is refused before it is bound. */
	CHECK(db_users_get_by_key(ctx, DB_USERS_SERIAL(UINT64_MAX), &row) == SQLITE_RANGE);

	/* The system table, and a view's row type. */
	db_sys_state_row state = {0};

	CHECK(db_sys_state_get_by_key(ctx, DB_SYS_STATE_ID(1), &state) == SQLITE_DONE);
	db_sys_state_row_free(&state);
	db_users_row_free(&row);

	db_ctx_close(ctx);
	sqlite3_close(db);
	queries();
	printf("plan_test: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
