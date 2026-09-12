select json_object('input', ?1 ->> '$.usage.input_tokens', 'output', ?1 ->> '$.usage.output_tokens', 'cache_read', ?1 ->> '$.usage.cache_read_input_tokens')
