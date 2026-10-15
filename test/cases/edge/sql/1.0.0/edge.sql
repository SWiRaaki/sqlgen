-- The rowid alias, and a default whose text contains the word that used to fool detection.
create table alias_pk (id integer primary key, note text not null default 'autoincrement', created_at integer not null);
create table serial_pk (serial integer primary key autoincrement, created_at integer not null);

-- Primary keys that are not the rowid: each gets an index, and in a rowid table each may hold NULL.
create table desc_pk (id integer primary key desc, v int);
create table int_pk (id int primary key, v int);
create table text_pk (k text primary key, v blob);
create table wr (k text primary key, n integer not null, v) without rowid;
create table st (id integer primary key, k text not null unique, a any, r real default 1.5) strict;

-- A junction declared by a composite primary key: one key over both, never one per column.
create table pair (
	a integer not null references serial_pk,
	b integer not null references alias_pk(id),
	primary key (a, b)
);

-- Unique over nullable columns: not total, so not a junction either.
create table loose (a integer references serial_pk, b integer references alias_pk, unique (a, b));

create table gen (
	id     integer primary key,
	w      integer not null default (40 + 2),
	twice  integer generated always as (w * 2) virtual,
	thrice integer generated always as (w * 3) stored
);

create table composite_parent (x integer not null, y text not null, created_at integer, primary key (x, y));
create table composite_child (
	id integer primary key,
	px integer,
	py text,
	foreign key (px, py) references composite_parent on delete cascade
);

-- Names that need quoting, and indexes no getter can use.
create table idx (id integer primary key, "order" text, "with space" text, sqlitedata int, created_at integer);
create unique index ux_idx_lower on idx(lower("order"));
create unique index ux_idx_partial on idx("with space") where "with space" is not null;
create index ix_idx_desc on idx(created_at desc, "order" collate nocase);

create table dangling (id integer primary key, other integer references nowhere(id));

-- Legal: only names beginning with "sqlite_" are reserved, and a LIKE filter would hide this one.
create table sqlitedata (id integer primary key);

create trigger t_idx after insert on idx begin select 1; end;
