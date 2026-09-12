select 1 + count(*) from bric_log where job = ?1 and key = ?2 and kind in ('close', 'error')
