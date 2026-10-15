create table good (
	serial     integer primary key autoincrement,
	name       text    not null unique,
	created_at integer not null,
	updated_at integer
) strict;

create table parent (id integer primary key, code text, created_at integer not null) strict;

create table bad_target (
	id         integer primary key,
	code       text references parent(code),
	created_at integer not null
) strict;
create index ix_bad_target_code on bad_target(code);

create table orphan (id integer primary key, other integer references nowhere, created_at integer not null) strict;
create index ix_orphan_other on orphan(other);

create table link (
	a integer not null references good,
	b integer not null references parent,
	unique (a, b)
) strict;

create table loose (id integer primary key, email text unique, created_at integer not null) strict;

create table lax (id integer primary key, created_at integer not null);

create table wr (k text primary key, created_at integer not null) without rowid;
create table nopk (v text, created_at integer not null) strict;
create table twokey (a integer not null, b integer not null, created_at integer not null, primary key (a, b)) strict;
create table descpk (id integer primary key desc, created_at integer not null) strict;

create table wrong_serial (id integer primary key autoincrement, created_at integer not null) strict;
create table wrong_id (serial integer primary key, created_at integer not null) strict;

create table no_created (id integer primary key) strict;
create table only_updated (id integer primary key, updated_at integer) strict;

create table stamps (id integer primary key, created_at text not null, seen_at real) strict;

create table sys_rogue (id integer primary key, created_at integer not null) strict;

create view v_good as select serial, name from good;
