create table if not exists sessions (
	serial     integer primary key asc autoincrement,

	user       integer not null,

	family     text    not null,

	token_hash text    not null,

	expires_at integer not null,

	created_at integer not null,

	updated_at integer     null,

	unique( token_hash ),
	constraint fk_user foreign key ( user ) references users( serial ) on delete cascade
);

create index if not exists ix_sessions_family on sessions( family );
