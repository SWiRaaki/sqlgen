select s.serial, s.family, s.expires_at, u.username
from sessions s join users u on u.serial = s.user
where s.user = :user and s.expires_at > :now;
