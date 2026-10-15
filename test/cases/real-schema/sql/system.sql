create table if not exists sys_schema_patch (
	serial      integer primary key asc autoincrement,
	major       integer not null,
	minor       integer not null,
	patch       integer not null,
	prerelease  text    not null default '',
	build       text    not null default '',

	applied_by  text    not null,
	created_at  integer not null,

	unique( major, minor, patch, prerelease )
);

create table if not exists sys_schema_script (
	serial     integer primary key asc autoincrement,
	patch      integer not null,
	ordinal    integer not null,
	name       text    not null,
	sql        text    not null,
	created_at integer not null,

	unique( patch, ordinal ),
	constraint fk_patch foreign key ( patch ) references sys_schema_patch( serial )
		on delete cascade
);

create table if not exists sys_instance (
	id         integer primary key check ( id = 1 ),
	uuid       text    not null,
	created_at integer not null
);
