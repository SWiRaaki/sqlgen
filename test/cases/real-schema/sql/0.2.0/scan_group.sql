create table if not exists scan_group (
	serial      integer primary key asc autoincrement,

	source      text    not null,
	upstream_id text    not null,
	name        text    not null,

	created_at  integer not null,
	updated_at  integer     null,

	unique( source, upstream_id )
);
