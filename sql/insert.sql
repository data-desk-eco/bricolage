select 'insert into "' || ?1 || '" ("key", ' || group_concat('"' || name || '"') || ') select ?1, '
  || group_concat('json_extract(?2, ''$."' || name || '"'')')
from pragma_table_info(?1) where name != 'key'
