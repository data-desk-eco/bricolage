static const char sql_attempt[] =
    "select 1 + count(*) from bric_log where job = ?1 and key = ?2 and kind in ('close', 'error')";

static const char sql_calls[] =
    "select json_group_array(json(value)) from json_each(?1, '$.content') where value ->> 'type' = 'tool_use'";

static const char sql_closed[] =
    "select 1 from bric_log where job = ?1 and key = ?2 and kind = 'close'";

static const char sql_count[] =
    "select count(*) from json_each(?1)";

static const char sql_ddl[] =
    "select sql from sqlite_schema where name = ?1 and type = 'table'";

static const char sql_dead[] =
    "insert or ignore into bric_log (job, key, attempt, kind, detail)"
    " select job, key, attempt, 'error', 'dead: pid ' || coalesce(max(iif(kind = 'open', detail ->> 'pid', null)), 'unknown') || ' gone'"
    " from bric_log"
    " where job = ?1 and key = ?2 and attempt is not null"
    " group by attempt"
    " having sum(kind in ('close', 'error')) = 0 and not alive(max(iif(kind = 'open', detail ->> 'pid', null)))";

static const char sql_field[] =
    "select ?1 -> ?2 ->> ?3";

static const char sql_image[] =
    "select json_array(json_object('type', 'image', 'source', json_object('type', 'base64', 'media_type', ?1, 'data', ?2)))";

static const char sql_job[] =
    "select target, brief, shell from bric_job where source = ?1";

static const char sql_kind[] =
    "select kind from bric_log where job = ?1 and key = ?2 and attempt = ?3 and kind in ('close', 'error')";

static const char sql_log[] =
    "insert into bric_log (job, key, attempt, turn, kind, tool, detail, text, usage)"
    " values (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, json(?9))"
    " returning seq";

static const char sql_message[] =
    "select json_insert(?1, '$[#]', json_object('role', ?2, 'content', json(?3)))";

static const char sql_no_call[] =
    "select 'reply without submission: ' || coalesce(group_concat(value ->> 'text', char(10)), ?1 ->> '$.stop_reason')"
    " from json_each(?1, '$.content') where value ->> 'type' = 'text'";

static const char sql_open[] =
    "insert or ignore into bric_log (job, key, attempt, turn, kind, detail)"
    " select ?1, ?2, ?3, 0, 'open', json_object('system', ?4, 'pid', ?6, 'tools', json(?7), 'shell', ?8)"
    " where cast(?5 as integer) > (select count(*) from ("
    " select 1 from bric_log where attempt is not null group by job, key, attempt"
    " having sum(kind in ('close', 'error')) = 0 and alive(max(iif(kind = 'open', detail ->> 'pid', null)))))";

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

static const char sql_receipt_shown[] =
    "select json_group_array(json(value))"
    " from ("
    " select json_object('type', 'text', 'text',"
    " '[seq ' || ?1 || '] ' || substr(?2, 1, ?3)"
    " || iif(length(?2) > cast(?3 as integer), char(10) || '... ' || (length(?2) - ?3) || ' more characters: select substr(text, ' || (?3 + 1) || ') from bric_log where seq = ' || ?1, '')"
    " ) as value"
    " union all"
    " select value from json_each(?4)"
    " )";

static const char sql_request[] =
    "select json_object('model', ?1, 'max_tokens', 8192, 'system', ?2, 'tools', json(?3), 'messages', json(?4))";

static const char sql_result[] =
    "select json_object('type', 'tool_result', 'tool_use_id', ?1, 'content', iif(json_valid(?2), json(?2), ?2))";

static const char sql_row[] =
    "select printf('select json_object(%s) from \"%w\" where \"key\" = ?1',"
    " group_concat(printf('%Q, iif(typeof(\"%w\") = ''blob'', cast(\"%w\" as text), \"%w\")', name, name, name, name)), ?1)"
    " from pragma_table_info(?1)";

static const char sql_schema[] =
    "create table if not exists bric_job ("
    " source text primary key,"
    " target text not null,"
    " brief  text not null,"
    " shell  text"
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
    " create virtual table if not exists bric_page using fts5 (text, content = 'bric_log', content_rowid = 'seq');"
    " "
    " create trigger if not exists bric_page_index after insert on bric_log when new.text is not null"
    " begin insert into bric_page (rowid, text) values (new.seq, new.text); end;"
    " "
    " create trigger if not exists bric_log_update before update on bric_log"
    " begin select raise(abort, 'bric_log is append-only'); end;"
    " "
    " create trigger if not exists bric_log_delete before delete on bric_log"
    " begin select raise(abort, 'bric_log is append-only'); end;"
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

static const char sql_sh_tool[] =
    "select json_object('name', 'sh',"
    " 'description', 'run a posix shell script. the receipt is stdout and stderr merged, then [exit N] when the status is not zero. '"
    " || 'the working directory is a scratch directory kept for this attempt, so files persist between calls. stdout that is a png or jpeg is shown to you as an image. '"
    " || '`obscura fetch URL --dump markdown --quiet` reads a page through a browser (--dump text|links|html; --screenshot p.png, then `cat p.png` to look at it). '"
    " || '`sqlite3 \"$BRIC_DB\"` is the research database. bric_page is fts5 over every page any attempt here has read'"
    " || ' (`select rowid, snippet(bric_page, 0, '''', '''', '' ... '', 48) from bric_page where bric_page match ''x''`); its rowid is a seq you may cite as if you had read the page.'"
    " || ' bric_log holds the full text of any receipt (`select text from bric_log where seq = N`). a receipt over 20000 characters is cut; page it from bric_log or narrow the script''s output. '"
    " || 'your key is the first user message, verbatim. your answer is a row in ' || ?1 || ' with that key: insert it with sqlite3 against the ddl in the system prompt. a constraint or trigger failure is your receipt, so correct and retry. '"
    " || 'a column naming a source takes the seq of the receipt whose own text contains your quote (a bric_page rowid is such a seq; web_search results have none). '"
    " || 'once a row for your key exists at the end of a turn you are done',"
    " 'input_schema', json_object('type', 'object', 'properties', json_object('script', json_object('type', 'string')), 'required', json_array('script')))";

static const char sql_usage[] =
    "select json_object('input', ?1 ->> '$.usage.input_tokens', 'output', ?1 ->> '$.usage.output_tokens', 'cache_read', ?1 ->> '$.usage.cache_read_input_tokens')";
