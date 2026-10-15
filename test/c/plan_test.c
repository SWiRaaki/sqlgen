/* Drives the C generated for test/cases/plan against a real database: every kind of statement,
 * the return codes, row ownership, cursors, typed ids and the embedded scripts. */

#include <stdio.h>
#include <string.h>

#include "db.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("plan_test:%d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static int apply(sqlite3 *db, const db_script *script) {
	char *message = NULL;
	int   rc      = sqlite3_exec(db, script->sql, NULL, NULL, &message);

	if (rc != SQLITE_OK) {
		printf("plan_test: %s: %s\n", script->name, message);
	}

	sqlite3_free(message);
	return rc;
}

static int text_is(const db_text *text, const char *expected) {
	return text->ptr && text->len == strlen(expected) && strcmp(text->ptr, expected) == 0;
}

int main(void) {
	sqlite3 *db  = NULL;
	db_ctx  *ctx = NULL;

	CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
	CHECK(sqlite3_exec(db, "pragma foreign_keys = on", NULL, NULL, NULL) == SQLITE_OK);

	/* The schema, from the scripts the generator embedded. */
	CHECK(db_systables != NULL && apply(db, db_systables) == SQLITE_OK);
	CHECK(db_patch_count == 1 && strcmp(db_patches[0].version, "1.0.0") == 0 && db_patches[0].major == 1);

	for (size_t p = 0; p < db_patch_count; ++p) {
		for (size_t s = 0; s < db_patches[p].count; ++s) {
			CHECK(apply(db, &db_patches[p].scripts[s]) == SQLITE_OK);
		}
	}

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
	printf("plan_test: %s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
