select json_object(
  'name', 'submit',
  'description', 'insert the row for this key into ' || ?1 || '. sqlite validates it against the ddl in the system prompt; an error is your receipt, so correct and resubmit. '
    || 'a column naming a source takes the number from the [seq N] head of the tool receipt whose own text contains your quote verbatim (not the navigation''s seq, and never a web_search result, which has none)',
  'input_schema', json_object(
    'type', 'object',
    'properties', json_group_object(name, json_object('type',
      case when type like '%int%' then 'integer'
           when type like '%rea%' or type like '%flo%' or type like '%dou%' or type like '%num%' then 'number'
           else 'string' end)),
    'required', (select json_group_array(name) from pragma_table_info(?1) where "notnull" and name != 'key')))
from pragma_table_info(?1) where name != 'key'
