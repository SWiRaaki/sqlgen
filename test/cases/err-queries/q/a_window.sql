select slug, rank() over (order by "order") as position from tag;
