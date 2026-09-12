select json_object('type', 'tool_result', 'tool_use_id', ?1, 'content', iif(json_valid(?2), json(?2), ?2))
