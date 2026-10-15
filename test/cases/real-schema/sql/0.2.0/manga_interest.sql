create table if not exists manga_interest (
	serial      integer primary key asc autoincrement,

	manga       integer not null,

	views       integer not null default 1,

	created_at  integer not null,
	updated_at  integer     null,

	unique( manga ),

	constraint fk_manga foreign key ( manga ) references manga( serial ) on delete cascade
);

create index if not exists ix_manga_interest_seen on manga_interest( updated_at, created_at );
