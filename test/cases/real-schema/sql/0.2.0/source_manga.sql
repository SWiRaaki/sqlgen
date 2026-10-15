create table if not exists source_manga (
	serial              integer primary key asc autoincrement,

	manga               integer not null,
	source              text    not null,
	upstream_id         text    not null,

	title               text    not null,
	description         text        null,
	cover_file          text        null,
	status              text        null,
	content_rating      text        null,
	original_language   text        null,
	year                integer     null,

	fetched_at          integer     null,
	fetched_chapters_at integer     null,

	created_at          integer not null,
	updated_at          integer     null,

	unique( source, upstream_id ),
	unique( manga, source ),

	constraint fk_manga foreign key ( manga ) references manga( serial ) on delete cascade
);
