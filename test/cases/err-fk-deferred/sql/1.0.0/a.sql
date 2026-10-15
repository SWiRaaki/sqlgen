create table p (id integer primary key);
create table c (id integer primary key, p integer references p(id) deferrable initially deferred);
insert into c (id, p) values (1, 99);
