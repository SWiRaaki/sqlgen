create table if not exists linked_accounts (
	serial        integer primary key asc autoincrement,

	user          integer not null,

	source        text    not null,

	external_id   text        null,

	access_token  text        null,
	refresh_token text        null,
	expires_at    integer     null,

	created_at    integer not null,
	updated_at    integer     null,

	unique( user, source ),

	constraint fk_user foreign key ( user ) references users( serial ) on delete cascade
);
