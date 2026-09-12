static const char sql_attempt[] =
    "select 1 + count(*) from bric_log where job = ?1 and key = ?2 and kind in ('close', 'error')";

static const char sql_calls[] =
    "select json_group_array(json(value)) from json_each(?1, '$.content') where value ->> 'type' = 'tool_use'";

static const char sql_text[] =
    "update bric_log set text = ?2 where seq = ?1";

static const char sql_closed[] =
    "select 1 from bric_log where job = ?1 and key = ?2 and kind = 'close'";

static const char sql_concat[] =
    "select json_group_array(json(value)) from (select value from json_each(?1) union all select value from json_each(?2))";

static const char sql_count[] =
    "select count(*) from json_each(?1)";

static const char sql_ddl[] =
    "select sql from sqlite_schema where name = ?1 and type = 'table'";

static const char sql_dead[] =
    "insert or ignore into bric_log (job, key, attempt, kind, detail)"
    " select job, key, attempt, 'error', 'dead: last row at ' || max(ts)"
    " from bric_log"
    " where job = ?1 and key = ?2 and attempt is not null"
    " group by attempt"
    " having sum(kind in ('close', 'error')) = 0 and max(ts) < datetime('now', (-2 * ?3) || ' seconds')";

static const char sql_field[] =
    "select ?1 -> ?2 ->> ?3";

static const char sql_insert[] =
    "select 'insert into \"' || ?1 || '\" (\"key\", ' || group_concat('\"' || name || '\"') || ') select ?1, '"
    " || group_concat('json_extract(?2, ''$.\"' || name || '\"'')')"
    " from pragma_table_info(?1) where name != 'key'";

static const char sql_job[] =
    "select target, brief, coalesce(tools, ?2) from bric_job where source = ?1";

static const char sql_job_set[] =
    "insert or replace into bric_job (source, target, brief, tools) values (?1, ?2, ?3, ?4)";

static const char sql_kind[] =
    "select kind from bric_log where job = ?1 and key = ?2 and attempt = ?3 and kind in ('close', 'error')";

static const char sql_listed[] =
    "select json_group_array(json_object('name', value ->> 'name', 'description', value ->> 'description', 'input_schema', value -> 'inputSchema'))"
    " from json_each(?1, '$.result.tools')"
    " where ?2 is null or value ->> 'name' in (select value from json_each(?2))";

static const char sql_log[] =
    "insert into bric_log (job, key, attempt, turn, kind, tool, detail, usage)"
    " values (?1, ?2, ?3, ?4, ?5, ?6, ?7, json(?8))"
    " returning seq";

static const char sql_mcp_call[] =
    "select json_object('jsonrpc', '2.0', 'id', 0, 'method', 'tools/call', 'params', json_object('name', ?1, 'arguments', json(?2)))";

static const char sql_message[] =
    "select json_insert(?1, '$[#]', json_object('role', ?2, 'content', json(?3)))";

static const char sql_no_call[] =
    "select 'reply without submission: ' || coalesce(group_concat(value ->> 'text', char(10)), ?1 ->> '$.stop_reason')"
    " from json_each(?1, '$.content') where value ->> 'type' = 'text'";

static const char sql_open[] =
    "insert or ignore into bric_log (job, key, attempt, turn, kind, detail)"
    " select ?1, ?2, ?3, 0, 'open', json_object('system', ?4)"
    " where ?5 > (select count(*) from ("
    " select 1 from bric_log where attempt is not null group by job, key, attempt"
    " having sum(kind in ('close', 'error')) = 0 and max(ts) >= datetime('now', (-2 * ?6) || ' seconds')))";

static const char sql_open_tools[] =
    "update bric_log set detail = json_set(detail, '$.tools', json(?4))"
    " where job = ?1 and key = ?2 and attempt = ?3 and kind = 'open'";

static const char sql_opened[] =
    "select 1 from bric_log where job = ?1 and key = ?2 and attempt = ?3 and kind = 'open'";

static const char sql_push[] =
    "select json_insert(?1, '$[#]', json(?2))";

static const char sql_quote[] =
    "select json_quote(?1)";

static const char sql_receipt_detail[] =
    "select length(?1) || ' chars: '"
    " || substr(?1, 1, min(120, coalesce(nullif(instr(?1, char(10)), 0) - 1, 120)))"
    " || coalesce((select '; ' || count(*) || ' image ' || sum(length(value ->> '$.source.data')) || ' chars' from json_each(?2)), '')";

static const char sql_receipt_images[] =
    "select json_group_array(json_object('type', 'image', 'source', json_object('type', 'base64', 'media_type', value ->> 'mimeType', 'data', value ->> 'data')))"
    " from json_each(?1, '$.result.content') where value ->> 'type' = 'image'"
    " having count(*) > 0";

static const char sql_receipt_shown[] =
    "select json_group_array(json(value))"
    " from ("
    " select json_object('type', 'text', 'text',"
    " '[seq ' || ?1 || '] ' || substr(?2, 1, ?3)"
    " || iif(length(?2) > cast(?3 as integer), char(10) || '... ' || (length(?2) - ?3) || ' more characters; page with the tool', '')"
    " ) as value"
    " union all"
    " select value from json_each(?4)"
    " )";

static const char sql_receipt_text[] =
    "select coalesce("
    " 'error: ' || (?1 ->> '$.error.message'),"
    " (select group_concat(iif(value ->> 'type' = 'text', value ->> 'text', '[' || (value ->> 'type') || ' dropped]'), char(10))"
    " from json_each(?1, '$.result.content') where value ->> 'type' != 'image'),"
    " iif(?1 -> '$.result.content' is not null, '', 'error: ' || ?2 || ' ' || coalesce(?1, '')))";

static const char sql_request[] =
    "select json_object('model', ?1, 'max_tokens', 8192, 'system', ?2, 'tools', json(?3), 'messages', json(?4))";

static const char sql_result[] =
    "select json_object('type', 'tool_result', 'tool_use_id', ?1, 'content', iif(json_valid(?2), json(?2), ?2))";

static const char sql_route[] =
    "select value from json_each(?1) where key = ?2";

static const char sql_routes[] =
    "select json_patch(?1, (select json_group_object(value ->> 'name', ?3) from json_each(?2)))";

static const char sql_schema[] =
    "create table if not exists bric_job ("
    " source text primary key,"
    " target text not null,"
    " brief  text not null,"
    " tools  text"
    ");"
    " "
    " create table if not exists bric_log ("
    " seq     integer primary key,"
    " ts      text not null default (datetime('now')),"
    " job     text not null,"
    " key     text,"
    " attempt integer,"
    " turn    integer,"
    " kind    text not null,"
    " tool    text,"
    " detail  text,"
    " text    text,"
    " usage   text"
    " ) strict;"
    " "
    " create index if not exists bric_log_job on bric_log (job, key);"
    " "
    " create unique index if not exists bric_claim on bric_log (job, key, attempt, kind)"
    " where kind in ('open', 'close', 'error');"
    " "
    " create view if not exists bric_attempt as"
    " select l.job, l.key, l.attempt, l.turn, l.ts, l.kind, l.tool, l.detail, u.input, u.output, u.cache_read"
    " from bric_log as l"
    " join ("
    " select job, key, max(seq) as seq,"
    " sum(usage ->> 'input') as input,"
    " sum(usage ->> 'output') as output,"
    " sum(usage ->> 'cache_read') as cache_read"
    " from bric_log"
    " group by job, key"
    " ) as u using (seq);"
    " "
    " create view if not exists bric_transcript as"
    " select job, key, attempt, json_group_array(json(message)) as messages"
    " from ("
    " select job, key, attempt, seq, json_object('role', 'user', 'content', key) as message"
    " from bric_log where kind = 'open'"
    " union all"
    " select job, key, attempt, seq, json_object('role', 'assistant', 'content', json(detail))"
    " from bric_log where kind = 'reply'"
    " union all"
    " select job, key, attempt, min(seq), json_object('role', 'user', 'content', json_group_array(json_object("
    " 'type', 'tool_result', 'tool_use_id', id, 'content', '[seq ' || seq || '] ' || coalesce(text, detail))))"
    " from ("
    " select l.job, l.key, l.attempt, l.turn, l.seq, l.text, l.detail,"
    " (select id from (select value ->> 'id' as id, row_number() over () - 1 as m from json_each(r.detail) where value ->> 'type' = 'tool_use') where m = l.n) as id"
    " from ("
    " select *, row_number() over (partition by job, key, attempt, turn order by seq) - 1 as n"
    " from bric_log where kind = 'receipt'"
    " ) as l"
    " join bric_log as r using (job, key, attempt, turn)"
    " where r.kind = 'reply'"
    " ) group by job, key, attempt, turn"
    " order by seq"
    " ) group by job, key, attempt;";

static const char sql_spec[] =
    "select iif(json_valid(?1), ?1, json_array(?1))";

static const char sql_spec_names[] =
    "select iif(json_type(?1) = 'array', null, (select json(value) from json_each(?1) limit 1 offset ?2))";

static const char sql_spec_url[] =
    "select iif(json_type(?1) = 'array', json_extract(?1, '$[' || ?2 || ']'), (select key from json_each(?1) limit 1 offset ?2))";

static const char sql_submit_tool[] =
    "select json_object("
    " 'name', 'submit',"
    " 'description', 'insert the row for this key into ' || ?1 || '. sqlite validates it against the ddl in the system prompt; an error is your receipt, so correct and resubmit. '"
    " || 'a column naming a source takes the number from the [seq N] head of the tool receipt whose own text contains your quote verbatim (not the navigation''s seq, and never a web_search result, which has none)',"
    " 'input_schema', json_object("
    " 'type', 'object',"
    " 'properties', json_group_object(name, json_object('type',"
    " case when type like '%int%' then 'integer'"
    " when type like '%rea%' or type like '%flo%' or type like '%dou%' or type like '%num%' then 'number'"
    " else 'string' end)),"
    " 'required', (select json_group_array(name) from pragma_table_info(?1) where \"notnull\" and name != 'key')))"
    " from pragma_table_info(?1) where name != 'key'";

static const char sql_usage[] =
    "select json_object('input', ?1 ->> '$.usage.input_tokens', 'output', ?1 ->> '$.usage.output_tokens', 'cache_read', ?1 ->> '$.usage.cache_read_input_tokens')";
