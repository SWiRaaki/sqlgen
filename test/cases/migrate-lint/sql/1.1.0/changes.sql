drop table logs;
alter table keep drop column extra;
alter table tags rename to labels;
alter table labels rename column slug to name;
create unique index ux_keep_v on keep(v);

-- The rebuild SQLite's documentation describes: age becomes an integer, nick NOT NULL.
create table users_new (
	serial integer primary key autoincrement,
	name   text not null,
	nick   text not null default '',
	age    integer
);
insert into users_new select serial, name, coalesce(nick, ''), cast(age as integer) from users;
drop table users;
alter table users_new rename to users;

-- Created and changed within the patch: nothing existed to migrate.
create table fresh (id integer primary key);
alter table fresh add column note text not null default '';
create unique index ux_fresh_note on fresh(note);
