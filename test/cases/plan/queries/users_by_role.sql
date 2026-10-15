-- Every user with a role, alphabetically.
select * from users where role = :role order by username;
