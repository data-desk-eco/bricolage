select json_group_array(json_object('name', value ->> 'name', 'description', value ->> 'description', 'input_schema', value -> 'inputSchema'))
from json_each(?1, '$.result.tools')
where ?2 is null or value ->> 'name' in (select value from json_each(?2))
