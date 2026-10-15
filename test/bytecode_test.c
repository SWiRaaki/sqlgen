/* What the bytecode analysis concludes for each query below, against the schema the arguments
 * load. The Makefile compares this output with test/bytecode.expected. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "bytecode.h"

static const char *const QUERIES[] = {
	/* a join, a left join, a cast, ordering */
	"select c.serial, c.title, g.name as group_name, cast(c.number as real) as number from chapter c"
	" left join scan_group g on g.serial = c.scan_group where c.source_manga = :series and c.language = :lang"
	" order by number",
	/* aggregates, functions, arithmetic, a group */
	"select language, count(*) as n, max(page_count) as most, sum(page_count) as total, avg(page_count) as mean,"
	" 1 + page_count as next, 'x' || title as label, length(title) as len, coalesce(volume, 'none') as vol,"
	" page_count / :d as ratio, page_count > 3 as big from chapter where source_manga = :s group by language",
	/* a correlated subquery, coalesce in a condition */
	"select sm.serial, sm.upstream_id from source_manga sm join manga_interest mi on mi.manga = sm.manga"
	" where sm.source = :source and coalesce(mi.updated_at, mi.created_at) >= :since and not exists"
	" (select 1 from stats_history sh where sh.source_manga = sm.serial and sh.created_at > :sampled)"
	" order by coalesce(mi.updated_at, mi.created_at) desc",
	/* at most one row: a unique key, a rowid, LIMIT 1 */
	"select serial, title from chapter where source_manga = :s and upstream_id = :u",
	"select * from users where serial = :id",
	"select serial from chapter where title = ? order by serial limit 1",
	/* exactly one row */
	"select count(*) as n from chapter where source_manga = :s",
	"select upper(:x) as shout",
	/* comparisons without an index */
	"select title from chapter where page_count >= :min",
	/* writes */
	"insert into sessions (user, family, token_hash, expires_at, created_at) values (:user, :family, :hash, :expires, :now)",
	"update sessions set updated_at = :now where token_hash = :hash",
	"delete from sessions where expires_at <= :now",
	"insert into chapter (source_manga, upstream_id, number, volume, title, language, scan_group, page_count,"
	" external_url, published_at, created_at) values (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) returning serial",
	/* distinct, a scalar subquery */
	"select distinct language from chapter where source_manga = :s",
	"select m.slug, (select count(*) from source_manga sm where sm.manga = m.serial) as copies from manga m",
	/* more expression forms */
	"select case when page_count > 10 then 'long' else 'short' end as size, case when volume is null then 1 end as"
	" missing, iif(page_count > 1, title, null) as titled, nullif(language, 'en') as other, title like :pattern as hit"
	" from chapter",
	"select exists (select 1 from chapter where source_manga = :s) as has, json_extract(:doc, '$.a') as a",
	"select title from chapter where scan_group in (select serial from scan_group where name = :group)",
	/* compound and common table expressions */
	"select username as name from users union all select display_text from users",
	"with recent as (select serial, title from chapter where published_at > :since) select title from recent",
	/* a NULL inside the row loop is data, even beside a NOT NULL column */
	"select iif(page_count > 1, language, null) as maybe, case when page_count > 1 then language end as also from chapter",
	/* a window function the analysis does not type */
	"select title, row_number() over (order by serial) as n from chapter",
	NULL
};

static void types(unsigned t) {
	static const char *const names[] = { "null", "int", "real", "text", "blob", "UNKNOWN" };
	bool                     first   = true;

	for (int i = 1; i < 6; ++i) {
		if (t & (1u << i)) {
			printf("%s%s", first ? "" : "|", names[i]);
			first = false;
		}
	}

	printf("%s%s", first ? "-" : "", t & SG_BT_NULL ? "?" : "!");
}

static void source(sg_bsource s) {
	if (s.table) {
		printf(" <%s.%s>", s.table->name, s.column >= 0 ? s.table->columns.items[s.column].name : "rowid");
	}
}

static char *slurp(const char *path) {
	FILE *f = fopen(path, "rb");

	if (!f || fseek(f, 0, SEEK_END) != 0) {
		perror(path);
		exit(2);
	}

	long  n    = ftell(f);
	char *text = malloc((size_t)n + 1);

	rewind(f);

	if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
		perror(path);
		exit(2);
	}

	text[n] = '\0';
	fclose(f);
	return text;
}

int main(int argc, char **argv) {
	static const char *const rows[] = { "none", "at most one", "exactly one", "many" };
	sqlite3                 *db     = NULL;
	sg_model                 model;

	sqlite3_open(":memory:", &db);

	for (int i = 1; i < argc; ++i) {
		char *text    = slurp(argv[i]);
		char *message = NULL;

		if (sqlite3_exec(db, text, NULL, NULL, &message) != SQLITE_OK) {
			printf("%s: %s\n", argv[i], message);
			return 2;
		}

		free(text);
	}

	if (!sg_introspect(db, &model)) {
		return 2;
	}

	for (size_t q = 0; QUERIES[q]; ++q) {
		sg_banalysis  a;
		sqlite3_stmt *stmt = NULL;

		printf("\n%s\n", QUERIES[q]);

		if (!sg_bytecode_analyze(db, &model, QUERIES[q], "query", &a)) {
			printf("  not analysed\n");
			continue;
		}

		sqlite3_prepare_v2(db, QUERIES[q], -1, &stmt, NULL);
		printf("  rows: %s\n", rows[a.rows]);

		for (size_t i = 0; i < a.results.count; ++i) {
			printf("  column %s ", sqlite3_column_name(stmt, (int)i));
			types(a.results.items[i].types);
			source(a.results.items[i].source);
			printf("\n");
		}

		for (size_t i = 0; i < a.params.count; ++i) {
			const char *name = sqlite3_bind_parameter_name(stmt, (int)i + 1);

			printf("  param %s ", name ? name : "?");
			types(a.params.items[i].types);
			source(a.params.items[i].source);
			printf("%s\n", a.params.items[i].several ? " (several)" : "");
		}

		sqlite3_finalize(stmt);
		sg_banalysis_free(&a);
	}

	sg_model_free(&model);
	sqlite3_close(db);
	return 0;
}
