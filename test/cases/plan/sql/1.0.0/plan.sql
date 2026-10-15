create table users (
	serial       integer primary key autoincrement,
	username     text    not null unique,
	email        text    not null unique,
	display_text text,
	role         text    not null default 'reader',
	created_at   integer not null,
	updated_at   integer
);

create table sessions (
	serial     integer primary key autoincrement,
	user       integer not null references users,
	family     text    not null,
	token_hash text    not null unique,
	expires_at integer not null,
	created_at integer not null
);
create index ix_sessions_user_family on sessions(user, family);

create table tag (id integer primary key, slug text not null unique, "order" integer not null default 0);

create table user_tag (
	user integer not null references users,
	tag  integer not null references tag,
	primary key (user, tag)
) without rowid;
create index ix_user_tag_tag on user_tag(tag);

create table setting (name text primary key, value text not null) without rowid;

create table doubled (id integer primary key, w integer not null, twice integer generated always as (w * 2) virtual);

create view v_names as select username from users;
