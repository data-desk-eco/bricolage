select json_patch(?1, (select json_group_object(value ->> 'name', ?3) from json_each(?2)))
