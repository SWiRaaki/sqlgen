create table if not exists manga (
	serial      integer primary key asc autoincrement,

	slug        text    not null,

	created_at  integer not null,

	unique( slug )
);
