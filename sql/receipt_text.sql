select coalesce(
  'error: ' || (?1 ->> '$.error.message'),
  (select group_concat(iif(value ->> 'type' = 'text', value ->> 'text', '[' || (value ->> 'type') || ' dropped]'), char(10))
   from json_each(?1, '$.result.content') where value ->> 'type' != 'image'),
  iif(?1 -> '$.result.content' is not null, '', 'error: ' || ?2 || ' ' || coalesce(?1, '')))
