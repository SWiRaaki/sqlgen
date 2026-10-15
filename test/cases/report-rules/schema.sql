create table parent (id integer primary key, name text not null unique);
create table child (id integer primary key, parent integer not null references parent(id));
