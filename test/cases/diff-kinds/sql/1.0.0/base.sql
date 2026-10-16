create table owner (id integer primary key, name text not null);
create table item (
	id    integer primary key,
	owner integer not null references owner,
	label text not null,
	price real default 0
);
create index ix_item_label on item(label) where price > 0;
create table note (id integer primary key, body text);
create view v_items as select id, label from item;
create trigger tr_item_touch after update on item begin select 1; end;
create trigger tr_note_gone after delete on note begin select 1; end;
