create table if not exists tag_source (
	serial      integer primary key asc autoincrement,

	source      text    not null,
	upstream_id text    not null,

	name        text    not null,

	canonical   integer     null,

	created_at  integer not null,
	updated_at  integer     null,

	unique( source, upstream_id ),

	constraint fk_canonical foreign key ( canonical ) references tag_canonical( serial )
		on delete set null
);

create index if not exists ix_tag_source_canonical on tag_source( canonical );
