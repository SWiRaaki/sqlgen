#include "schema.h"

#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool is_blank(char c) {
	return c == ' ' || c == '\t' || c == '\r';
}

static bool escapes_directory(const char *name) {
	if (name[0] == '/') {
		return true;
	}

	for (const char *part = name; *part;) {
		size_t len = strcspn(part, "/");

		if (len == 2 && part[0] == '.' && part[1] == '.') {
			return true;
		}

		part += len + (part[len] == '/');
	}

	return false;
}

static bool load_manifest(const char *dir, sg_patch *patch) {
	char   *text = NULL;
	size_t  len  = 0;
	bool    seen = false;
	bool    ok   = true;

	patch->dir      = sg_xstrdup(dir);
	patch->manifest = sg_path_join(dir, "patch.manifest");

	if (!sg_read_file(patch->manifest, &text, &len)) {
		return false;
	}

	unsigned line  = 0;
	size_t   start = 0;

	while (start < len) {
		size_t end = start;

		while (end < len && text[end] != '\n') {
			++end;
		}

		++line;

		size_t first = start;
		size_t last  = end;

		while (first < last && is_blank(text[first])) {
			++first;
		}

		while (last > first && is_blank(text[last - 1])) {
			--last;
		}

		size_t next = end + 1;

		if (first == last || text[first] == '#') {
			start = next;
			continue;
		}

		if (!seen) {
			size_t bad = 0;

			if (!sg_semver_parse(text + first, last - first, &patch->version, &bad)) {
				sg_error(patch->manifest, line, (unsigned)(first - start + bad + 1),
						"'%.*s' is not a semantic version", (int)(last - first), text + first);
				ok = false;
				break;
			}

			seen  = true;
			start = next;
			continue;
		}

		char *name = sg_xstrndup(text + first, last - first);
		bool  dup  = false;

		for (size_t i = 0; i < patch->scripts.count; ++i) {
			dup = dup || strcmp(patch->scripts.items[i].name, name) == 0;
		}

		if (escapes_directory(name)) {
			sg_error(patch->manifest, line, (unsigned)(first - start + 1),
					"'%s' leaves the patch directory; scripts must be inside it", name);
			ok = false;
			free(name);
		} else if (dup) {
			sg_error(patch->manifest, line, (unsigned)(first - start + 1),
					"'%s' is listed twice", name);
			ok = false;
			free(name);
		} else {
			sg_script *script = SG_PUSH(patch->scripts);

			script->name = name;
			script->path = sg_path_join(dir, name);
			script->line = line;

			if (!sg_is_file(script->path)) {
				sg_error(patch->manifest, line, (unsigned)(first - start + 1),
						"'%s' does not exist", name);
				ok = false;
			}
		}

		start = next;
	}

	free(text);

	if (ok && !seen) {
		sg_error(patch->manifest, 0, 0, "names no version");
		ok = false;
	} else if (ok && patch->scripts.count == 0) {
		sg_error(patch->manifest, 0, 0, "names no scripts");
		ok = false;
	}

	return ok;
}

static bool known_dir(const sg_patchset *set, const char *dir) {
	for (size_t i = 0; i < set->patches.count; ++i) {
		if (strcmp(set->patches.items[i].dir, dir) == 0) {
			return true;
		}
	}

	return false;
}

static bool find_pattern(const sg_config *config, const char *pattern, sg_patchset *out) {
	glob_t found = {0};
	int    rc    = glob(pattern, GLOB_MARK, NULL, &found);
	bool   ok    = true;
	size_t dirs  = 0;

	if (rc == GLOB_NOMATCH) {
		sg_error(config->path, 0, 0, "$.schema.patches: '%s' matches nothing", pattern);
		globfree(&found);
		return false;
	}

	if (rc != 0) {
		sg_error(config->path, 0, 0, "$.schema.patches: '%s' could not be expanded", pattern);
		globfree(&found);
		return false;
	}

	for (size_t i = 0; i < found.gl_pathc; ++i) {
		const char *match = found.gl_pathv[i];
		size_t      len   = strlen(match);

		/* GLOB_MARK appends '/' to directories; anything else the pattern caught is not a patch. */
		if (len < 2 || match[len - 1] != '/') {
			continue;
		}

		char *dir = sg_xstrndup(match, len - 1);

		++dirs;

		if (known_dir(out, dir)) {
			free(dir);
			continue;
		}

		char *manifest = sg_path_join(dir, "patch.manifest");

		if (!sg_is_file(manifest)) {
			sg_error(dir, 0, 0, "is matched by '%s' but holds no patch.manifest", pattern);
			ok = false;
		} else if (!load_manifest(dir, SG_PUSH(out->patches))) {
			ok = false;
		}

		free(manifest);
		free(dir);
	}

	if (dirs == 0) {
		sg_error(config->path, 0, 0, "$.schema.patches: '%s' matches no directory", pattern);
		ok = false;
	}

	globfree(&found);
	return ok;
}

bool sg_patches_find(const sg_config *config, sg_patchset *out) {
	bool ok = true;

	memset(out, 0, sizeof *out);
	out->systables = config->systables;

	if (config->systables && !sg_is_file(config->systables)) {
		sg_error(config->path, 0, 0, "$.schema.systables: '%s' does not exist", config->systables);
		ok = false;
	}

	for (size_t i = 0; i < config->patch_patterns.count; ++i) {
		ok = find_pattern(config, config->patch_patterns.items[i], out) && ok;
	}

	if (!ok) {
		return false;
	}

	sg_patch *items = out->patches.items;

	for (size_t i = 1; i < out->patches.count; ++i) {
		for (size_t j = i; j > 0 && sg_semver_compare(&items[j - 1].version, &items[j].version) > 0; --j) {
			sg_patch held = items[j - 1];

			items[j - 1] = items[j];
			items[j]     = held;
		}
	}

	for (size_t i = 1; i < out->patches.count; ++i) {
		if (sg_semver_compare(&items[i - 1].version, &items[i].version) == 0) {
			sg_error(items[i].manifest, 0, 0, "version %s has the same precedence as %s in %s;"
					" build metadata does not distinguish patches", items[i].version.text,
					items[i - 1].version.text, items[i - 1].manifest);
			ok = false;
		}
	}

	return ok;
}

static bool exec_plain(sqlite3 *db, const char *sql, const char *file) {
	char *message = NULL;

	if (sqlite3_exec(db, sql, NULL, NULL, &message) != SQLITE_OK) {
		sg_error(file, 0, 0, "%s: %s", sql, message ? message : sqlite3_errmsg(db));
		sqlite3_free(message);
		return false;
	}

	return true;
}

/* The first byte of the next statement: whitespace and both comment forms skipped. */
static size_t statement_start(const char *text, size_t at, size_t end) {
	while (at < end) {
		char c = text[at];

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
			++at;
		} else if (c == '-' && at + 1 < end && text[at + 1] == '-') {
			while (at < end && text[at] != '\n') {
				++at;
			}
		} else if (c == '/' && at + 1 < end && text[at + 1] == '*') {
			at += 2;

			while (at + 1 < end && !(text[at] == '*' && text[at + 1] == '/')) {
				++at;
			}

			at = at + 1 < end ? at + 2 : end;
		} else {
			break;
		}
	}

	return at;
}

static void error_at(const char *path, const char *text, size_t offset, const char *message) {
	unsigned line;
	unsigned col;

	sg_line_col(text, offset, &line, &col);
	sg_error(path, line, col, "%s", message);
}

/* Names starting with this belong to the tables the generated migration keeps. */
#define RESERVED_PREFIX "_sys_"

/* Compared without regard to case, as SQLite compares names. */
static bool reserved(const char *name) {
	const char *prefix = RESERVED_PREFIX;

	for (size_t i = 0; prefix[i]; ++i) {
		char c = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] - 'A' + 'a') : name[i];

		if (c != prefix[i]) {
			return false;
		}
	}

	return true;
}

/* Records which objects the last statement created; returns the name of one it created under the
 * reserved prefix, from malloc, or NULL. */
static char *record_origins(sqlite3 *db, sg_patchset *set, sg_origin origin) {
	sqlite3_stmt *stmt     = NULL;
	char         *forbidden = NULL;

	for (size_t i = 0; i < set->origins.count; ++i) {
		set->origins.items[i].seen = false;
	}

	sqlite3_prepare_v2(db, "select type, name from sqlite_schema"
			" where type in ('table', 'view', 'index', 'trigger')"
			" and substr(name, 1, 7) <> 'sqlite_' collate nocase", -1, &stmt, NULL);

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		const char       *type  = (const char *)sqlite3_column_text(stmt, 0);
		const char       *name  = (const char *)sqlite3_column_text(stmt, 1);
		sg_object_origin *found = NULL;

		for (size_t i = 0; i < set->origins.count && !found; ++i) {
			sg_object_origin *known = &set->origins.items[i];

			if (strcmp(known->type, type) == 0 && strcmp(known->name, name) == 0) {
				found = known;
			}
		}

		if (!found) {
			found         = SG_PUSH(set->origins);
			found->type   = sg_xstrdup(type);
			found->name   = sg_xstrdup(name);
			found->origin = origin;

			if (!forbidden && reserved(name)) {
				forbidden = sg_xstrdup(name);
			}
		}

		found->seen = true;
	}

	sqlite3_finalize(stmt);

	/* Whatever a script dropped is forgotten, so a later script recreating it becomes its origin. */
	size_t kept = 0;

	for (size_t i = 0; i < set->origins.count; ++i) {
		if (set->origins.items[i].seen) {
			set->origins.items[kept++] = set->origins.items[i];
		} else {
			free(set->origins.items[i].type);
			free(set->origins.items[i].name);
		}
	}

	set->origins.count = kept;
	return forbidden;
}

/* Origins are recorded after every statement that writes, not once per script: a script that drops
 * and recreates a table would otherwise leave the table's first creator as its origin. */
static int schema_version(sqlite3 *db) {
	sqlite3_stmt *stmt    = NULL;
	int           version = -1;

	if (sqlite3_prepare_v2(db, "pragma schema_version", -1, &stmt, NULL) == SQLITE_OK
			&& sqlite3_step(stmt) == SQLITE_ROW) {
		version = sqlite3_column_int(stmt, 0);
	}

	sqlite3_finalize(stmt);
	return version;
}

static bool snapshot(sqlite3 *db, sg_patchset *set, sg_origin at) {
	sg_snapshot *shot = SG_PUSH(set->history);

	shot->at     = at;
	set->version = schema_version(db);

	if (!sg_introspect_between(db, &shot->model)) {
		sg_error(at.script, at.line, at.col, "the schema could not be read after this statement: %s",
				sqlite3_errmsg(db));
		return false;
	}

	return true;
}

/* Only a statement that changed the schema adds to its history; inserting seed rows does not. */
static bool snapshot_if_changed(sqlite3 *db, sg_patchset *set, sg_origin at) {
	return schema_version(db) == set->version || snapshot(db, set, at);
}

static bool apply_script(sqlite3 *db, const char *path, sg_patchset *set, sg_origin origin) {
	char   *text = NULL;
	size_t  len  = 0;

	if (!sg_read_file(path, &text, &len)) {
		return false;
	}

	const char *nul = memchr(text, '\0', len);

	/* SQLite stops at a NUL even when given a length, so whatever followed would never run. */
	if (nul) {
		error_at(path, text, (size_t)(nul - text), "contains a NUL byte");
		free(text);
		return false;
	}

	if (len > INT_MAX) {
		sg_error(path, 0, 0, "is too large");
		free(text);
		return false;
	}

	size_t at = len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
	bool   ok = true;

	while (at < len) {
		sqlite3_stmt *stmt = NULL;
		const char   *tail = NULL;
		const char   *from = text + at;

		if (sqlite3_prepare_v2(db, from, (int)(len - at), &stmt, &tail) != SQLITE_OK) {
			int offset = sqlite3_error_offset(db);

			error_at(path, text, offset >= 0 ? at + (size_t)offset : statement_start(text, at, len),
					sqlite3_errmsg(db));
			ok = false;
			break;
		}

		if (!stmt) {
			break;
		}

		int rc;

		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		}

		if (rc != SQLITE_DONE) {
			error_at(path, text, statement_start(text, at, len), sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			ok = false;
			break;
		}

		bool writes = !sqlite3_stmt_readonly(stmt);

		sqlite3_finalize(stmt);

		if (writes) {
			sg_origin here = origin;

			sg_line_col(text, statement_start(text, at, len), &here.line, &here.col);

			char *forbidden = record_origins(db, set, here);

			if (!forbidden && !origin.system && !snapshot_if_changed(db, set, here)) {
				ok = false;
				break;
			}

			if (forbidden) {
				char *message = sg_format("%s starts with " RESERVED_PREFIX ", which is reserved for the"
						" tables the generated migration keeps", forbidden);

				error_at(path, text, statement_start(text, at, len), message);
				free(message);
				free(forbidden);
				ok = false;
				break;
			}
		}

		if (sqlite3_get_autocommit(db)) {
			error_at(path, text, statement_start(text, at, len), "ends the transaction the script"
					" runs in; a script must not COMMIT, END or ROLLBACK");
			ok = false;
			break;
		}
		at = (size_t)(tail - text);
	}

	free(text);
	return ok;
}

static bool check_foreign_keys(sqlite3 *db) {
	sqlite3_stmt *stmt = NULL;
	bool          ok   = true;

	if (sqlite3_prepare_v2(db, "select \"table\", rowid, parent from pragma_foreign_key_check",
			-1, &stmt, NULL) != SQLITE_OK) {
		sg_error(NULL, 0, 0, "pragma foreign_key_check failed: %s", sqlite3_errmsg(db));
		return false;
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *table  = (const char *)sqlite3_column_text(stmt, 0);
		const char *parent = (const char *)sqlite3_column_text(stmt, 2);

		if (sqlite3_column_type(stmt, 1) == SQLITE_NULL) {
			sg_error(NULL, 0, 0, "a row of %s references a missing row of %s", table, parent);
		} else {
			sg_error(NULL, 0, 0, "row %lld of %s references a missing row of %s",
					(long long)sqlite3_column_int64(stmt, 1), table, parent);
		}

		ok = false;
	}

	sqlite3_finalize(stmt);
	return ok;
}

static bool run_script(sqlite3 *db, sg_patchset *set, const char *path, const char *patch,
		bool system) {
	if (!exec_plain(db, "begin", path)) {
		return false;
	}

	sg_origin origin = { path, patch, system, 0, 0 };

	if (!apply_script(db, path, set, origin)) {
		if (!sqlite3_get_autocommit(db)) {
			exec_plain(db, "rollback", path);
		}

		return false;
	}

	/* A failed COMMIT leaves the transaction open, which is what lets the check see the rows. */
	if (sqlite3_exec(db, "commit", NULL, NULL, NULL) != SQLITE_OK) {
		sg_error(path, 0, 0, "cannot be committed: %s", sqlite3_errmsg(db));
		check_foreign_keys(db);
		exec_plain(db, "rollback", path);
		return false;
	}

	return true;
}

/* Every object's type, name and definition, as one text to compare. */
static char *schema_text(sqlite3 *db) {
	sqlite3_stmt *stmt = NULL;
	sg_buf        out  = {0};

	sqlite3_prepare_v2(db, "select type, name, coalesce(sql, '') from sqlite_schema order by type, name",
			-1, &stmt, NULL);

	while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
		for (int i = 0; i < 3; ++i) {
			sg_buf_puts(&out, (const char *)sqlite3_column_text(stmt, i));
			sg_buf_putc(&out, '\n');
		}
	}

	sqlite3_finalize(stmt);
	return out.data ? out.data : sg_xstrdup("");
}

/* The generated migration runs the system-tables script before every migration, so a second run
 * must succeed and change nothing: `create table if not exists`, `insert or ignore`. */
static bool systables_repeatable(sqlite3 *db, sg_patchset *set) {
	char *before = schema_text(db);
	bool  ok     = run_script(db, set, set->systables, NULL, true);

	if (!ok) {
		sg_error(set->systables, 0, 0, "fails when run a second time; it runs before every migration, so"
				" it must be repeatable (create … if not exists, insert or ignore)");
	} else {
		char *after = schema_text(db);

		if (strcmp(before, after) != 0) {
			sg_error(set->systables, 0, 0, "changes the schema when run a second time; it runs before"
					" every migration, so it must be repeatable");
			ok = false;
		}

		free(after);
	}

	free(before);
	return ok;
}

bool sg_schema_load(sqlite3 *db, const sg_config *config, sg_patchset *set) {
	int dqs = config->double_quoted_strings ? 1 : 0;

	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DDL, dqs, (int *)NULL);
	sqlite3_db_config(db, SQLITE_DBCONFIG_DQS_DML, dqs, (int *)NULL);

	if (!exec_plain(db, config->foreign_keys ? "pragma foreign_keys = on"
			: "pragma foreign_keys = off", NULL)) {
		return false;
	}

	if (set->systables && !run_script(db, set, set->systables, NULL, true)) {
		return false;
	}

	sg_origin base = { set->systables, NULL, true, 0, 0 };

	if (!snapshot(db, set, base)) {
		return false;
	}

	for (size_t i = 0; i < set->patches.count; ++i) {
		sg_patch *patch = &set->patches.items[i];

		for (size_t j = 0; j < patch->scripts.count; ++j) {
			if (!run_script(db, set, patch->scripts.items[j].path, patch->version.text, false)) {
				return false;
			}
		}

		patch->end = set->history.count - 1;
	}

	if (set->systables && !systables_repeatable(db, set)) {
		return false;
	}

	return !config->foreign_keys || check_foreign_keys(db);
}

static const sg_origin *find_origin(const sg_patchset *set, const char *type, const char *name) {
	for (size_t i = 0; i < set->origins.count; ++i) {
		const sg_object_origin *known = &set->origins.items[i];

		if (strcmp(known->type, type) == 0 && strcmp(known->name, name) == 0) {
			return &known->origin;
		}
	}

	return NULL;
}

void sg_schema_attach_origins(const sg_patchset *set, sg_model *model) {
	for (size_t i = 0; i < model->tables.count; ++i) {
		sg_table        *table  = &model->tables.items[i];
		const sg_origin *origin = find_origin(set, table->kind == SG_VIEW ? "view" : "table",
				table->name);

		if (origin) {
			table->created = *origin;
		}

		for (size_t j = 0; j < table->indexes.count; ++j) {
			sg_index        *index = &table->indexes.items[j];
			const sg_origin *made  = find_origin(set, "index", index->name);

			/* An automatic index comes from a constraint in the table's own definition, which no
			 * later statement can add to. */
			index->created = made ? *made : table->created;
		}
	}
}

void sg_patchset_free(sg_patchset *set) {
	for (size_t i = 0; i < set->patches.count; ++i) {
		sg_patch *patch = &set->patches.items[i];

		sg_semver_free(&patch->version);
		free(patch->dir);
		free(patch->manifest);

		for (size_t j = 0; j < patch->scripts.count; ++j) {
			free(patch->scripts.items[j].name);
			free(patch->scripts.items[j].path);
		}

		free(patch->scripts.items);
	}

	free(set->patches.items);

	for (size_t i = 0; i < set->origins.count; ++i) {
		free(set->origins.items[i].type);
		free(set->origins.items[i].name);
	}

	free(set->origins.items);

	for (size_t i = 0; i < set->history.count; ++i) {
		sg_model_free(&set->history.items[i].model);
	}

	free(set->history.items);
	memset(set, 0, sizeof *set);
}
