select json_group_array(json(value))
from (
  select json_object('type', 'text', 'text',
    '[seq ' || ?1 || '] ' || substr(?2, 1, ?3)
    || iif(length(?2) > cast(?3 as integer), char(10) || '... ' || (length(?2) - ?3) || ' more characters; page with the tool', '')
  ) as value
  union all
  select value from json_each(?4)
)
