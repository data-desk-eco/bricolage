select json_group_array(json(value)) from json_each(?1, '$.content') where value ->> 'type' = 'tool_use'
