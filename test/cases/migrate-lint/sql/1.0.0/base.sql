create table users (
	serial integer primary key autoincrement,
	name   text not null,
	nick   text,
	age    text
);
create table logs (id integer primary key, line text);
create table tags (id integer primary key, slug text);
create table keep (id integer primary key, v text not null, extra text);
create index ix_keep_v on keep(v);
