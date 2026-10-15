create table if not exists source_manga_artist (
	source_manga integer not null,
	person       integer not null,

	unique( source_manga, person ),

	constraint fk_source_manga foreign key ( source_manga ) references source_manga( serial )
		on delete cascade,
	constraint fk_person foreign key ( person ) references person( serial ) on delete cascade
);
