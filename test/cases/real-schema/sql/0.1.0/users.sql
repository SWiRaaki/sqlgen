create table if not exists users (

	serial           integer primary key asc autoincrement,

	username         text    not null,
	email            text    not null,

	display_text     text    not null,

	password         text    not null,

	deactivated_at   integer     null,

	created_at       integer not null,
	updated_at       integer     null,

	unique( username ),
	unique( email )
);
