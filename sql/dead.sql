insert or ignore into bric_log (job, key, attempt, kind, detail)
select job, key, attempt, 'error', 'dead: last row at ' || max(ts)
from bric_log
where job = ?1 and key = ?2 and attempt is not null
group by attempt
having sum(kind in ('close', 'error')) = 0 and max(ts) < datetime('now', (-2 * ?3) || ' seconds')
