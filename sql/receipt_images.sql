select json_group_array(json_object('type', 'image', 'source', json_object('type', 'base64', 'media_type', value ->> 'mimeType', 'data', value ->> 'data')))
from json_each(?1, '$.result.content') where value ->> 'type' = 'image'
having count(*) > 0
