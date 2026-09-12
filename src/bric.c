#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#include <curl/curl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sqlite3 *L;

static const char *env(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

static char *q(int *rc, const char *sql, ...)
{
    sqlite3_stmt *s;
    char *out = NULL;
    va_list ap;
    int r = sqlite3_prepare_v2(L, sql, -1, &s, NULL);
    va_start(ap, sql);
    for (int i = 1; !r && i <= sqlite3_bind_parameter_count(s); i++)
        sqlite3_bind_text(s, i, va_arg(ap, const char *), -1, SQLITE_TRANSIENT);
    va_end(ap);
    if (!r) {
        r = sqlite3_step(s);
        if (r == SQLITE_ROW && sqlite3_column_text(s, 0))
            out = sqlite3_mprintf("%s", sqlite3_column_text(s, 0));
        r = r == SQLITE_ROW || r == SQLITE_DONE ? SQLITE_OK : r;
    }
    sqlite3_finalize(s);
    if (rc) *rc = r;
    return out;
}

static void squeeze(char *s)
{
    char *w = s;
    int space = 0;
    for (char *r = s; *r; r++) {
        if (!strncmp(r, "data:", 5)) {
            char *e = r;
            while (*e && !strchr(" )\n\"'", *e)) e++;
            for (char *p = r; p + 8 <= e; p++)
                if (!memcmp(p, ";base64,", 8)) { r = e - 1; break; }
            if (r == e - 1) continue;
        }
        if (isspace((unsigned char)*r)) {
            space = *r == '\n' || space == 2 ? 2 : 1;
            continue;
        }
        if (space && w > s) *w++ = space == 2 ? '\n' : ' ';
        space = 0;
        *w++ = *r;
    }
    *w = 0;
}

static void squeeze_fn(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    char *s = sqlite3_mprintf("%s", sqlite3_value_text(argv[0]) ? (const char *)sqlite3_value_text(argv[0]) : "");
    squeeze(s);
    sqlite3_result_text(ctx, s, -1, sqlite3_free);
}

typedef struct {
    const char *url, *body;
    int inference;
    char *out, *err;
    size_t n;
    long status;
} Req;

static struct session { char *url, *id; struct session *next; } *sessions;

static char **session(const char *url)
{
    struct session *s;
    for (s = sessions; s; s = s->next)
        if (!strcmp(s->url, url)) return &s->id;
    s = sqlite3_malloc(sizeof *s);
    s->url = sqlite3_mprintf("%s", url);
    s->id = NULL;
    s->next = sessions;
    sessions = s;
    return &s->id;
}

static size_t on_body(char *p, size_t size, size_t n, Req *r)
{
    r->out = sqlite3_realloc(r->out, r->n + size * n + 1);
    memcpy(r->out + r->n, p, size * n);
    r->n += size * n;
    r->out[r->n] = 0;
    return size * n;
}

static size_t on_header(char *p, size_t size, size_t n, Req *r)
{
    if (size * n > 15 && !sqlite3_strnicmp(p, "mcp-session-id:", 15)) {
        char **id = session(r->url);
        sqlite3_free(*id);
        *id = sqlite3_mprintf("%.*s", (int)(size * n - 15), p + 15);
        squeeze(*id);
    }
    return size * n;
}

static void http(Req *r, int n)
{
    CURLM *m = curl_multi_init();
    CURL *h[n];
    struct curl_slist *hs[n];
    int live;
    for (int i = 0; i < n; i++) {
        char *auth = r[i].inference ? sqlite3_mprintf("x-api-key: %s", env("BRIC_KEY", ""))
                   : *session(r[i].url) ? sqlite3_mprintf("mcp-session-id: %s", *session(r[i].url)) : NULL;
        r[i].out = r[i].err = NULL;
        r[i].n = r[i].status = 0;
        hs[i] = curl_slist_append(NULL, "content-type: application/json");
        hs[i] = curl_slist_append(hs[i], "accept: application/json, text/event-stream");
        hs[i] = curl_slist_append(hs[i], "anthropic-version: 2023-06-01");
        if (auth) hs[i] = curl_slist_append(hs[i], auth);
        sqlite3_free(auth);
        h[i] = curl_easy_init();
        curl_easy_setopt(h[i], CURLOPT_URL, r[i].url);
        curl_easy_setopt(h[i], CURLOPT_POSTFIELDS, r[i].body);
        curl_easy_setopt(h[i], CURLOPT_HTTPHEADER, hs[i]);
        curl_easy_setopt(h[i], CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(h[i], CURLOPT_WRITEDATA, &r[i]);
        curl_easy_setopt(h[i], CURLOPT_HEADERFUNCTION, on_header);
        curl_easy_setopt(h[i], CURLOPT_HEADERDATA, &r[i]);
        curl_easy_setopt(h[i], CURLOPT_TIMEOUT, atol(env("BRIC_TIMEOUT", "120")));
        curl_easy_setopt(h[i], CURLOPT_PRIVATE, &r[i]);
        curl_multi_add_handle(m, h[i]);
    }
    do {
        curl_multi_perform(m, &live);
        curl_multi_poll(m, NULL, 0, 1000, NULL);
    } while (live);
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(m, &live))) {
        Req *x;
        curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &x);
        if (msg->data.result) x->err = sqlite3_mprintf("%s", curl_easy_strerror(msg->data.result));
        else curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &x->status);
    }
    for (int i = 0; i < n; i++) {
        curl_multi_remove_handle(m, h[i]);
        curl_easy_cleanup(h[i]);
        curl_slist_free_all(hs[i]);
    }
    curl_multi_cleanup(m);
}

static char *unsse(char *body)
{
    char *data = NULL;
    if (!body || *body == '{') return body;
    for (char *p = body; (p = strstr(p, "data:")); p += 5) data = p + 5;
    if (!data) return body;
    while (*data == ' ') data++;
    char *end = strchr(data, '\n');
    if (end) *end = 0;
    return data;
}

static char *infer(const char *body, char **err)
{
    Req r = { env("BRIC_URL", "https://api.anthropic.com/v1/messages"), body, 1 };
    for (int i = 0;; i++) {
        http(&r, 1);
        if (!r.err && r.status < 400) return r.out;
        int retry = r.err || r.status == 429 || r.status >= 500;
        if (!retry || i == 3) {
            *err = r.err ? r.err : sqlite3_mprintf("%ld %s", r.status, r.out);
            if (r.err) sqlite3_free(r.out);
            return NULL;
        }
        sqlite3_free(r.out);
        sqlite3_free(r.err);
        sleep(2 << i);
    }
}

typedef struct {
    const char *target, *brief, *key;
    char *attempt, *usage, *insert, *tools, *routes;
    int turn, done, n;
    char **seq, **text;
} Attempt;

static char *logrow(Attempt *a, const char *kind, const char *tool, const char *detail)
{
    char turn[16];
    snprintf(turn, sizeof turn, "%d", a->turn);
    char *seq = q(NULL, "insert into bric_log (job, key, attempt, turn, kind, tool, detail, usage) values (?1, ?2, ?3, ?4, ?5, ?6, ?7, json(?8)) returning seq",
                  a->brief, a->key, a->attempt, turn, kind, tool, detail, a->usage);
    sqlite3_free(a->usage);
    a->usage = NULL;
    a->done |= strcmp(kind, "call") && strcmp(kind, "receipt");
    return seq;
}

static char *receipt(Attempt *a, const char *tool, char *text, const char *images)
{
    squeeze(text);
    char *detail = q(NULL, "select length(?1) || ' chars: ' || substr(?1, 1, min(120, coalesce(nullif(instr(?1, char(10)), 0) - 1, 120))) || "
                           "coalesce((select '; ' || count(*) || ' image ' || sum(length(value ->> '$.source.data')) || ' chars' from json_each(?2)), '')", text, images);
    char *seq = logrow(a, "receipt", tool, detail);
    sqlite3_free(detail);
    a->seq = sqlite3_realloc(a->seq, ++a->n * sizeof *a->seq);
    a->text = sqlite3_realloc(a->text, a->n * sizeof *a->text);
    a->seq[a->n - 1] = seq;
    a->text[a->n - 1] = text;
    return q(NULL, "select json_group_array(json(value)) from (select json_object('type', 'text', 'text', '[seq ' || ?1 || '] ' || substr(?2, 1, ?3) "
                   "|| iif(length(?2) > cast(?3 as integer), char(10) || '... ' || (length(?2) - ?3) || ' more characters; page with the tool', '')) as value "
                   "union all select value from json_each(?4))", seq, text, "20000", images);
}

static char *submit(Attempt *a, const char *input)
{
    int rc;
    q(NULL, "savepoint bric");
    for (int i = 0; i < a->n; i++)
        q(NULL, "update bric_log set text = ?2 where seq = ?1 and exists (select 1 from json_each(?3) where type = 'integer' and value = cast(?1 as integer))", a->seq[i], a->text[i], input);
    q(&rc, a->insert, a->key, input);
    char *err = rc ? sqlite3_mprintf("%s", sqlite3_errmsg(L)) : NULL;
    if (rc) q(NULL, "rollback to bric");
    q(NULL, "release bric");
    return err;
}

static char *tools(Attempt *a, const char *spec)
{
    static char *cached_spec, *cached_tools, *cached_routes;
    if (!cached_spec || !spec || strcmp(cached_spec, spec)) {
        sqlite3_free(cached_spec);
        sqlite3_free(cached_tools);
        sqlite3_free(cached_routes);
        cached_spec = spec ? sqlite3_mprintf("%s", spec) : NULL;
        cached_tools = sqlite3_mprintf("[{\"type\":\"web_search_20250305\",\"name\":\"web_search\"}]");
        cached_routes = sqlite3_mprintf("{}");
        char *count = q(NULL, "select count(*) from json_each(?1)", spec);
        for (int i = 0; count && i < atoi(count); i++) {
            char index[16];
            snprintf(index, sizeof index, "%d", i);
            char *url = q(NULL, "select iif(json_type(?1) = 'array', json_extract(?1, '$[' || ?2 || ']'), (select key from json_each(?1) limit 1 offset ?2))", spec, index);
            char *names = q(NULL, "select iif(json_type(?1) = 'array', null, (select json(value) from json_each(?1) limit 1 offset ?2))", spec, index);
            Req r = { url, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"tools/list\"}", 0 };
            http(&r, 1);
            char *listed = unsse(r.out);
            if (r.err || r.status >= 400) {
                char *err = sqlite3_mprintf("%s: %s", url, r.err ? r.err : listed);
                sqlite3_free(cached_spec);
                cached_spec = NULL;
                return err;
            }
            char *found = q(NULL, "select json_group_array(json_object('name', value ->> 'name', 'description', value ->> 'description', 'input_schema', value -> 'inputSchema')) "
                                  "from json_each(?1, '$.result.tools') where ?2 is null or value ->> 'name' in (select value from json_each(?2))", listed, names);
            char *merged = q(NULL, "select json_group_array(json(value)) from (select value from json_each(?1) union all select value from json_each(?2))", cached_tools, found);
            char *routes = q(NULL, "select json_patch(?1, (select json_group_object(value ->> 'name', ?3) from json_each(?2)))", cached_routes, found, url);
            sqlite3_free(cached_tools);
            sqlite3_free(cached_routes);
            cached_tools = merged;
            cached_routes = routes;
            sqlite3_free(found);
            sqlite3_free(names);
            sqlite3_free(url);
            sqlite3_free(r.out);
        }
        sqlite3_free(count);
    }
    char *submit = q(NULL,
        "select json_object('name', 'submit', 'description', 'insert the row for this key into ' || ?1 || '. sqlite validates it against the ddl in the system prompt; an error is your receipt, so correct and resubmit. "
        "a column naming a source takes the number from the [seq N] head of the tool receipt whose own text contains your quote verbatim (not the navigation''s seq, and never a web_search result, which has none)', "
        "'input_schema', json_object('type', 'object', 'properties', json_group_object(name, json_object('type', case when type like '%int%' then 'integer' "
        "when type like '%rea%' or type like '%flo%' or type like '%dou%' or type like '%num%' then 'number' else 'string' end)), "
        "'required', (select json_group_array(name) from pragma_table_info(?1) where \"notnull\" and name != 'key'))) from pragma_table_info(?1) where name != 'key'", a->target);
    a->tools = q(NULL, "select json_insert(?1, '$[#]', json(?2))", cached_tools, submit);
    a->routes = cached_routes;
    sqlite3_free(submit);
    return NULL;
}

static void turn(Attempt *a, const char *system, char **messages)
{
    char *err = NULL;
    char *body = q(NULL, "select json_object('model', ?1, 'max_tokens', 8192, 'system', ?2, 'tools', json(?3), 'messages', json(?4))", env("BRIC_MODEL", ""), system, a->tools, *messages);
    char *reply = infer(body, &err);
    sqlite3_free(body);
    if (!reply) {
        sqlite3_free(logrow(a, "error", NULL, err));
        sqlite3_free(err);
        return;
    }
    a->usage = q(NULL, "select json_object('input', ?1 ->> '$.usage.input_tokens', 'output', ?1 ->> '$.usage.output_tokens', 'cache_read', ?1 ->> '$.usage.cache_read_input_tokens')", reply);
    char *next = q(NULL, "select json_insert(?1, '$[#]', json_object('role', 'assistant', 'content', ?2 -> '$.content'))", *messages, reply);
    sqlite3_free(*messages);
    *messages = next;
    char *calls = q(NULL, "select json_group_array(json(value)) from json_each(?1, '$.content') where value ->> 'type' = 'tool_use'", reply);
    char *count = q(NULL, "select json_array_length(?1)", calls);
    int n = atoi(count);
    sqlite3_free(count);
    if (!n) {
        char *text = q(NULL, "select 'reply without submission: ' || coalesce(group_concat(value ->> 'text', char(10)), ?1 ->> '$.stop_reason') from json_each(?1, '$.content') where value ->> 'type' = 'text'", reply);
        sqlite3_free(logrow(a, "error", NULL, text));
        sqlite3_free(text);
    }
    sqlite3_free(reply);
    Req r[n];
    char *name[n], *input[n], *id[n], *url[n];
    for (int i = 0; i < n; i++) {
        char index[16];
        snprintf(index, sizeof index, "$[%d]", i);
        name[i] = q(NULL, "select ?1 -> ?2 ->> 'name'", calls, index);
        input[i] = q(NULL, "select ?1 -> ?2 -> 'input'", calls, index);
        id[i] = q(NULL, "select ?1 -> ?2 ->> 'id'", calls, index);
        url[i] = q(NULL, "select value from json_each(?1) where key = ?2", a->routes, name[i]);
        sqlite3_free(logrow(a, "call", name[i], input[i]));
        r[i].url = url[i] ? url[i] : "";
        r[i].body = url[i] ? q(NULL, "select json_object('jsonrpc', '2.0', 'id', 0, 'method', 'tools/call', 'params', json_object('name', ?1, 'arguments', json(?2)))", name[i], input[i]) : "";
        r[i].inference = 0;
    }
    http(r, n);
    char *results = sqlite3_mprintf("[]");
    for (int i = 0; i < n && !a->done; i++) {
        char *text, *shown;
        if (!strcmp(name[i], "submit")) {
            text = submit(a, input[i]);
            if (!text) {
                sqlite3_free(logrow(a, "close", NULL, input[i]));
                break;
            }
            shown = sqlite3_mprintf("%s", text);
        } else if (!url[i]) {
            text = sqlite3_mprintf("unknown tool %s", name[i]);
            shown = sqlite3_mprintf("%s", text);
        } else {
            char *json = unsse(r[i].out), status[16];
            snprintf(status, sizeof status, "%ld", r[i].status);
            text = r[i].err ? sqlite3_mprintf("error: %s", r[i].err)
                 : q(NULL, "select coalesce('error: ' || (?1 ->> '$.error.message'), "
                           "(select group_concat(iif(value ->> 'type' = 'text', value ->> 'text', '[' || (value ->> 'type') || ' dropped]'), char(10)) from json_each(?1, '$.result.content') where value ->> 'type' != 'image'), "
                           "iif(?1 -> '$.result.content' is not null, '', 'error: ' || ?2 || ' ' || coalesce(?1, '')))", json, status);
            char *images = q(NULL, "select json_group_array(json_object('type', 'image', 'source', json_object('type', 'base64', 'media_type', value ->> 'mimeType', 'data', value ->> 'data'))) "
                                   "from json_each(?1, '$.result.content') where value ->> 'type' = 'image' having count(*) > 0", json);
            shown = receipt(a, name[i], text, images);
            sqlite3_free(images);
            text = NULL;
        }
        if (text) sqlite3_free(logrow(a, "receipt", name[i], text));
        char *more = q(NULL, "select json_insert(?1, '$[#]', json_object('type', 'tool_result', 'tool_use_id', ?2, 'content', iif(json_valid(?3), json(?3), ?3)))", results, id[i], shown);
        sqlite3_free(results);
        results = more;
        sqlite3_free(shown);
        sqlite3_free(text);
    }
    for (int i = 0; i < n; i++) {
        if (url[i]) sqlite3_free((char *)r[i].body);
        sqlite3_free(r[i].out);
        sqlite3_free(r[i].err);
        sqlite3_free(name[i]);
        sqlite3_free(input[i]);
        sqlite3_free(id[i]);
        sqlite3_free(url[i]);
    }
    if (!a->done) {
        next = q(NULL, "select json_insert(?1, '$[#]', json_object('role', 'user', 'content', json(?2)))", *messages, results);
        sqlite3_free(*messages);
        *messages = next;
    }
    sqlite3_free(results);
    sqlite3_free(calls);
}

static void run(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    Attempt a = { (const char *)sqlite3_value_text(argv[0]), (const char *)sqlite3_value_text(argv[1]), (const char *)sqlite3_value_text(argv[2]) };
    const char *spec = argc > 3 && sqlite3_value_type(argv[3]) != SQLITE_NULL ? (const char *)sqlite3_value_text(argv[3]) : env("BRIC_TOOLS", NULL);
    char *ddl = q(NULL, "select sql from sqlite_schema where name = ?1 and type = 'table'", a.target);
    if (!ddl) {
        sqlite3_result_error(ctx, "run: no such table", -1);
        return;
    }
    a.insert = q(NULL, "select 'insert into \"' || ?1 || '\" (\"key\", ' || group_concat('\"' || name || '\"') || ') select ?1, ' || group_concat('json_extract(?2, ''$.\"' || name || '\"'')') from pragma_table_info(?1) where name != 'key'", a.target);
    q(NULL, "insert or ignore into bric_log (job, key, attempt, kind, detail) "
            "select job, key, attempt, 'error', 'dead: last row at ' || max(ts) from bric_log where job = ?1 and key = ?2 and attempt is not null "
            "group by attempt having sum(kind in ('close', 'error')) = 0 and max(ts) < datetime('now', (-2 * ?3) || ' seconds')", a.brief, a.key, env("BRIC_TIMEOUT", "120"));
    a.attempt = q(NULL, "select 1 + count(*) from bric_log where job = ?1 and key = ?2 and kind in ('close', 'error')", a.brief, a.key);
    char *err = q(NULL, "select 1 from bric_log where job = ?1 and key = ?2 and kind = 'close'", a.brief, a.key) ? NULL : tools(&a, spec);
    int rc = 1;
    if (err) sqlite3_free(logrow(&a, "error", NULL, err));
    else if (!a.tools) sqlite3_result_text(ctx, "close", -1, SQLITE_STATIC);
    else q(&rc, "insert into bric_log (job, key, attempt, turn, kind, detail) values (?1, ?2, ?3, 0, 'open', json(?4))", a.brief, a.key, a.attempt, a.tools);
    sqlite3_free(err);
    if (!rc) {
        char *system = sqlite3_mprintf("%s\n\n%s", a.brief, ddl);
        char *messages = q(NULL, "select json_array(json_object('role', 'user', 'content', ?1))", a.key);
        int turns = atoi(env("BRIC_TURNS", "40"));
        for (a.turn = 1; a.turn <= turns && !a.done; a.turn++) turn(&a, system, &messages);
        if (!a.done) sqlite3_free(logrow(&a, "error", NULL, "turn cap"));
        sqlite3_free(system);
        sqlite3_free(messages);
    }
    char *kind = a.done ? q(NULL, "select kind from bric_log where job = ?1 and key = ?2 and attempt = ?3 and kind in ('close', 'error')", a.brief, a.key, a.attempt) : NULL;
    if (kind) sqlite3_result_text(ctx, kind, -1, sqlite3_free);
    for (int i = 0; i < a.n; i++) {
        sqlite3_free(a.seq[i]);
        sqlite3_free(a.text[i]);
    }
    sqlite3_free(a.seq);
    sqlite3_free(a.text);
    sqlite3_free(a.attempt);
    sqlite3_free(a.insert);
    sqlite3_free(a.tools);
    sqlite3_free(ddl);
}

static const char *ddl =
    "create table if not exists bric_log ("
    " seq integer primary key, ts text not null default (datetime('now')), job text not null, key text, attempt integer, turn integer,"
    " kind text not null, tool text, detail text, text text, usage text) strict;"
    "create unique index if not exists bric_claim on bric_log (job, key, attempt, kind) where kind in ('open', 'close', 'error');"
    "create view if not exists bric_attempt as"
    " select l.job, l.key, l.attempt, l.turn, l.ts, l.kind, l.tool, l.detail, u.input, u.output, u.cache_read"
    " from bric_log as l join (select job, key, max(seq) as seq, sum(usage ->> 'input') as input, sum(usage ->> 'output') as output, sum(usage ->> 'cache_read') as cache_read"
    " from bric_log group by job, key) as u using (seq);";

int sqlite3_bric_init(sqlite3 *db, char **err, const sqlite3_api_routines *api)
{
    SQLITE_EXTENSION_INIT2(api);
    (void)err;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const char *file = sqlite3_db_filename(db, "main");
    L = db;
    if (file && *file && sqlite3_open(file, &L) == SQLITE_OK) {
        sqlite3_busy_timeout(L, 30000);
        sqlite3_exec(L, "pragma journal_mode = wal", NULL, NULL, NULL);
    }
    sqlite3_exec(L, ddl, NULL, NULL, NULL);
    sqlite3_create_function(L, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "run", 3, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    sqlite3_create_function(db, "run", 4, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    return SQLITE_OK;
}
