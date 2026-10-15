create table if not exists source_manga_tag (
	source_manga integer not null,
	tag_source   integer not null,

	unique( source_manga, tag_source ),

	constraint fk_source_manga foreign key ( source_manga ) references source_manga( serial )
		on delete cascade,
	constraint fk_tag_source foreign key ( tag_source ) references tag_source( serial )
		on delete cascade
);
