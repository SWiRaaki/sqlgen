create view v_pair as
	select p.a, s.created_at as made, p.b + 1 as next
	from pair p join serial_pk s on s.serial = p.a;
