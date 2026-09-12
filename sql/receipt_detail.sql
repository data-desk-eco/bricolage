select length(?1) || ' chars: '
  || substr(?1, 1, min(120, coalesce(nullif(instr(?1, char(10)), 0) - 1, 120)))
  || coalesce((select '; ' || count(*) || ' image ' || sum(length(value ->> '$.source.data')) || ' chars' from json_each(?2)), '')
