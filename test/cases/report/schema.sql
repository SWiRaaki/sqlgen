create table parent (id integer primary key, name text not null unique);
create table child (id integer primary key autoincrement, parent integer not null references parent(id));
create index ix_child_parent on child(parent);
