alter table owner rename to person;
drop view v_items;

create table item_new (
	id    integer primary key,
	label text not null unique,
	owner integer not null references person on delete cascade,
	price real default 1,
	sku   text
) strict;
insert into item_new (id, label, owner, price) select id, label, owner, price from item;
drop table item;
alter table item_new rename to item;
create index ix_item_label on item(label) where price > 1;

create view v_items as select id, label, price from item;
create trigger tr_item_touch after update on item begin select 2; end;
drop trigger tr_note_gone;
create trigger tr_note_new after insert on note begin select 1; end;
