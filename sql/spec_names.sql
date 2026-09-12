select iif(json_type(?1) = 'array', null, (select json(value) from json_each(?1) limit 1 offset ?2))
