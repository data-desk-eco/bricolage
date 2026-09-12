select iif(json_type(?1) = 'array', json_extract(?1, '$[' || ?2 || ']'), (select key from json_each(?1) limit 1 offset ?2))
