select kind from bric_log where job = ?1 and key = ?2 and attempt = ?3 and kind in ('close', 'error')
