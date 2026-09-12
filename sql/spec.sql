select iif(json_valid(?1), ?1, json_array(?1))
