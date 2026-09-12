select json_group_array(json(value)) from (select value from json_each(?1) union all select value from json_each(?2))
