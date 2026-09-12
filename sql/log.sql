insert into bric_log (job, key, attempt, turn, kind, tool, detail, usage)
values (?1, ?2, ?3, ?4, ?5, ?6, ?7, json(?8))
returning seq
