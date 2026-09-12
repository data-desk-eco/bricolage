select json_object('model', ?1, 'max_tokens', 8192, 'system', ?2, 'tools', json(?3), 'messages', json(?4))
