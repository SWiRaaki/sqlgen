create table if not exists chapter (
	serial        integer primary key asc autoincrement,

	source_manga  integer not null,
	upstream_id   text    not null,

	number        text        null check ( number is null or number glob '[0-9]*' ),

	volume        text        null,
	title         text        null,
	language      text    not null,
	scan_group    integer     null,
	page_count    integer not null,

	external_url  text        null,

	published_at  integer     null,

	created_at    integer not null,
	updated_at    integer     null,

	unique( source_manga, upstream_id ),

	constraint fk_source_manga foreign key ( source_manga ) references source_manga( serial )
		on delete cascade,

	constraint fk_scan_group foreign key ( scan_group ) references scan_group( serial )
		on delete set null
);

create index if not exists ix_chapter_language on chapter( source_manga, language );
