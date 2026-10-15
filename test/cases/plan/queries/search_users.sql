-- @param pattern text
select serial, username from users where display_text like :pattern || '%';
