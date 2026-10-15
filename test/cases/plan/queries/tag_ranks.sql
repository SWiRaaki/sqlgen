-- @allow scan
-- @column position integer
-- @column slug text
select slug, rank() over (order by "order") as position from tag;
