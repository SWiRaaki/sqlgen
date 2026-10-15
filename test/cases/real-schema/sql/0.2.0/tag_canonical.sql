create table if not exists tag_canonical (
	serial      integer primary key asc autoincrement,

	slug        text    not null,

	grp         text    not null,

	created_at  integer not null,
	updated_at  integer     null,

	unique( slug )
);
