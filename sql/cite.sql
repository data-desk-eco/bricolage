update bric_log set text = ?2
where seq = ?1
  and exists (select 1 from json_each(?3) where type = 'integer' and value = cast(?1 as integer))
