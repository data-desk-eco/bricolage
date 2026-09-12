select 'reply without submission: ' || coalesce(group_concat(value ->> 'text', char(10)), ?1 ->> '$.stop_reason')
from json_each(?1, '$.content') where value ->> 'type' = 'text'
