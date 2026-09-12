select json_object('jsonrpc', '2.0', 'id', 0, 'method', 'tools/call', 'params', json_object('name', ?1, 'arguments', json(?2)))
