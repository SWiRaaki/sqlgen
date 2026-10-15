insert into users( serial, username, email, display_text, password, deactivated_at, created_at )
values

	( 1, 'root', 'root@localhost', 'root',
	  '', cast( unixepoch( 'now', 'subsec' ) * 1000 as integer ),
	      cast( unixepoch( 'now', 'subsec' ) * 1000 as integer ) ),

	( 2, 'api', 'api@localhost', 'api',
	  '', null, cast( unixepoch( 'now', 'subsec' ) * 1000 as integer ) );
