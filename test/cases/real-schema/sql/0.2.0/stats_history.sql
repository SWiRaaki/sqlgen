create table if not exists stats_history (
	serial        integer primary key asc autoincrement,

	source_manga  integer not null,

	follows       integer     null,

	rating        text        null,

	created_at    integer not null,

	constraint fk_source_manga foreign key ( source_manga ) references source_manga( serial )
		on delete cascade
);

create index if not exists ix_stats_history_taken on stats_history( source_manga, created_at );
