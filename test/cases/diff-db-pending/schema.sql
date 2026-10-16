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
create table _sys_patch (serial integer primary key autoincrement, major integer not null, minor integer not null, patch integer not null, prerelease text not null default '', build text not null default '', applied_by text not null, created_at integer not null, unique (major, minor, patch, prerelease));
insert into _sys_patch (major, minor, patch, applied_by, created_at) values (1, 0, 0, 'test', 0);
