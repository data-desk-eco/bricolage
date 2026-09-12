select json_insert(?1, '$[#]', json_object('role', ?2, 'content', json(?3)))
