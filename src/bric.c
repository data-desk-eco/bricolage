#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#include <curl/curl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
    sqlite3 *db, *file, *log;
    char *schema, *table;
} G;

static const char *env(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

static char *q(sqlite3 *c, int *rc, const char *sql, ...)
{
    sqlite3_stmt *s;
    char *out = NULL;
    va_list ap;
    int r = sqlite3_prepare_v2(c, sql, -1, &s, NULL);
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
        char *auth = r[i].inference ? sqlite3_mprintf("x-api-key: %s", env("ANTHROPIC_API_KEY", ""))
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
    sqlite3_vtab base;
    int columns;
    char *ddl, *submit, *insert, *read, *del, *yield, *spec, *tools, *routes;
} Table;

typedef struct {
    sqlite3_vtab_cursor base;
    sqlite3_stmt *s;
    int rc;
} Cursor;

typedef struct {
    Table *t;
    const char *brief, *key;
    char *attempt, *usage;
    int turn, done;
    int n;
    char **seq, **text;
} Attempt;

static char *logrow(Attempt *a, const char *kind, const char *tool, const char *detail)
{
    char turn[16];
    snprintf(turn, sizeof turn, "%d", a->turn);
    char *seq = q(G.log, NULL, "insert into main.bric_log (job, key, attempt, turn, kind, tool, detail, usage) "
                               "values (?1, ?2, ?3, ?4, ?5, ?6, ?7, json(?8)) returning seq",
                  a->brief, a->key, a->attempt, turn, kind, tool, detail, a->usage);
    sqlite3_free(a->usage);
    a->usage = NULL;
    a->done |= strcmp(kind, "call") && strcmp(kind, "receipt");
    return seq;
}

static char *receipt(Attempt *a, const char *tool, char *text)
{
    squeeze(text);
    char *detail = q(G.log, NULL, "select length(?1) || ' chars: ' || substr(?1, 1, min(120, coalesce(nullif(instr(?1, char(10)), 0) - 1, 120)))", text);
    char *seq = logrow(a, "receipt", tool, detail);
    sqlite3_free(detail);
    a->seq = sqlite3_realloc(a->seq, ++a->n * sizeof *a->seq);
    a->text = sqlite3_realloc(a->text, a->n * sizeof *a->text);
    a->seq[a->n - 1] = seq;
    a->text[a->n - 1] = text;
    char *shown = q(G.log, NULL, "select '[seq ' || ?1 || '] ' || substr(?2, 1, ?3) || iif(length(?2) > cast(?3 as integer), char(10) || '... ' || (length(?2) - ?3) || ' more characters; page with the tool', '')",
                    seq, text, env("BRIC_RECEIPT", "20000"));
    return shown;
}

static char *submit(Attempt *a, const char *raw, char **row)
{
    char *input = q(G.log, NULL, "select json_group_object(key, case type when 'text' then squeeze(value) when 'object' then json(value) when 'array' then json(value) else value end) from json_each(?1)", raw);
    for (int i = 0; i < a->n; i++) {
        char *cited = q(G.log, NULL, "select 1 from json_each(?1) where type = 'integer' and value = cast(?2 as integer)", input, a->seq[i]);
        if (cited) q(G.log, NULL, "update main.bric_log set text = ?2 where seq = ?1", a->seq[i], a->text[i]);
        sqlite3_free(cited);
    }
    int rc;
    q(G.db, &rc, a->t->insert, a->key, input);
    sqlite3_free(input);
    if (rc) return sqlite3_mprintf("%s", sqlite3_errmsg(G.db));
    char rowid[24];
    snprintf(rowid, sizeof rowid, "%lld", sqlite3_last_insert_rowid(G.db));
    *row = q(G.db, NULL, a->t->read, rowid);
    q(G.db, NULL, a->t->del, rowid);
    return NULL;
}

static char *tools(Table *t, const char *spec)
{
    if (t->spec && spec && !strcmp(t->spec, spec)) return NULL;
    sqlite3_free(t->spec);
    sqlite3_free(t->tools);
    sqlite3_free(t->routes);
    t->spec = spec ? sqlite3_mprintf("%s", spec) : NULL;
    t->tools = q(G.log, NULL, "select json_array(json(?1), json('{\"type\":\"web_search_20250305\",\"name\":\"web_search\"}'))", t->submit);
    t->routes = sqlite3_mprintf("{}");
    char *count = q(G.log, NULL, "select count(*) from json_each(?1)", spec);
    for (int i = 0; count && i < atoi(count); i++) {
        char index[16];
        snprintf(index, sizeof index, "%d", i);
        char *url = q(G.log, NULL, "select iif(json_type(?1) = 'array', json_extract(?1, '$[' || ?2 || ']'), (select key from json_each(?1) limit 1 offset ?2))", spec, index);
        char *names = q(G.log, NULL, "select iif(json_type(?1) = 'array', null, (select json(value) from json_each(?1) limit 1 offset ?2))", spec, index);
        Req r = { url, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"tools/list\"}", 0 };
        http(&r, 1);
        char *listed = unsse(r.out);
        if (r.err || r.status >= 400) {
            sqlite3_free(count);
            char *err = sqlite3_mprintf("%s: %s", url, r.err ? r.err : listed);
            sqlite3_free(r.err);
            sqlite3_free(r.out);
            return err;
        }
        char *found = q(G.log, NULL, "select json_group_array(json_object('name', value ->> 'name', 'description', value ->> 'description', 'input_schema', value -> 'inputSchema')) "
                                     "from json_each(?1, '$.result.tools') where ?2 is null or value ->> 'name' in (select value from json_each(?2))", listed, names);
        char *merged = q(G.log, NULL, "select json_group_array(json(value)) from (select value from json_each(?1) union all select value from json_each(?2))", t->tools, found);
        char *routes = q(G.log, NULL, "select json_patch(?1, (select json_group_object(value ->> 'name', ?3) from json_each(?2)))", t->routes, found, url);
        sqlite3_free(t->tools);
        sqlite3_free(t->routes);
        t->tools = merged;
        t->routes = routes;
        sqlite3_free(found);
        sqlite3_free(names);
        sqlite3_free(url);
        sqlite3_free(r.out);
    }
    sqlite3_free(count);
    return NULL;
}

static char *turn(Attempt *a, const char *system, char **messages)
{
    char *err = NULL, *row = NULL;
    char *body = q(G.log, NULL, "select json_object('model', ?1, 'max_tokens', 8192, 'system', ?2, 'tools', json(?3), 'messages', json(?4))",
                   env("BRIC_MODEL", ""), system, a->t->tools, *messages);
    char *reply = infer(body, &err);
    sqlite3_free(body);
    if (!reply) {
        sqlite3_free(logrow(a, "error", NULL, err));
        sqlite3_free(err);
        return NULL;
    }
    a->usage = q(G.log, NULL, "select json_object('input', ?1 ->> '$.usage.input_tokens', 'output', ?1 ->> '$.usage.output_tokens', 'cache_read', ?1 ->> '$.usage.cache_read_input_tokens')", reply);
    char *next = q(G.log, NULL, "select json_insert(?1, '$[#]', json_object('role', 'assistant', 'content', ?2 -> '$.content'))", *messages, reply);
    sqlite3_free(*messages);
    *messages = next;
    char *calls = q(G.log, NULL, "select json_group_array(json(value)) from json_each(?1, '$.content') where value ->> 'type' = 'tool_use'", reply);
    int n = atoi(q(G.log, NULL, "select json_array_length(?1)", calls));
    if (!n) {
        char *text = q(G.log, NULL, "select 'reply without submission: ' || coalesce(group_concat(value ->> 'text', char(10)), ?1 ->> '$.stop_reason') from json_each(?1, '$.content') where value ->> 'type' = 'text'", reply);
        sqlite3_free(logrow(a, "error", NULL, text));
        sqlite3_free(text);
        sqlite3_free(calls);
        sqlite3_free(reply);
        return NULL;
    }
    sqlite3_free(reply);
    Req r[n];
    char *name[n], *input[n], *id[n], *url[n];
    for (int i = 0; i < n; i++) {
        char index[16];
        snprintf(index, sizeof index, "$[%d]", i);
        name[i] = q(G.log, NULL, "select ?1 -> ?2 ->> 'name'", calls, index);
        input[i] = q(G.log, NULL, "select ?1 -> ?2 -> 'input'", calls, index);
        id[i] = q(G.log, NULL, "select ?1 -> ?2 ->> 'id'", calls, index);
        url[i] = q(G.log, NULL, "select value from json_each(?1) where key = ?2", a->t->routes, name[i]);
        sqlite3_free(logrow(a, "call", name[i], input[i]));
        r[i].url = url[i] ? url[i] : "";
        r[i].body = url[i] ? q(G.log, NULL, "select json_object('jsonrpc', '2.0', 'id', 0, 'method', 'tools/call', 'params', json_object('name', ?1, 'arguments', json(?2)))", name[i], input[i]) : "";
        r[i].out = r[i].err = NULL;
        r[i].inference = 0;
    }
    http(r, n);
    char *results = sqlite3_mprintf("[]");
    for (int i = 0; i < n; i++) {
        char *text, *shown;
        if (!strcmp(name[i], "submit")) {
            text = submit(a, input[i], &row);
            if (!text) {
                sqlite3_free(logrow(a, "close", NULL, row));
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
                 : q(G.log, NULL, "select coalesce('error: ' || (?1 ->> '$.error.message'), "
                                  "(select group_concat(iif(value ->> 'type' = 'text', value ->> 'text', '[' || (value ->> 'type') || ' dropped]'), char(10)) from json_each(?1, '$.result.content')), "
                                  "'error: ' || ?2 || ' ' || coalesce(?1, ''))", json, status);
            shown = receipt(a, name[i], text);
            text = NULL;
        }
        if (text) sqlite3_free(logrow(a, "receipt", name[i], text));
        char *next = q(G.log, NULL, "select json_insert(?1, '$[#]', json_object('type', 'tool_result', 'tool_use_id', ?2, 'content', ?3))", results, id[i], shown);
        sqlite3_free(results);
        results = next;
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
    if (!row) {
        char *next = q(G.log, NULL, "select json_insert(?1, '$[#]', json_object('role', 'user', 'content', json(?2)))", *messages, results);
        sqlite3_free(*messages);
        *messages = next;
    }
    sqlite3_free(results);
    sqlite3_free(calls);
    return row;
}

static char *attempt(Table *t, const char *brief, const char *key, const char *spec)
{
    char *row = q(G.log, NULL, "select detail from main.bric_log where job = ?1 and key = ?2 and kind = 'close' order by seq desc limit 1", brief, key);
    if (row) return row;
    q(G.log, NULL, "insert or ignore into main.bric_log (job, key, attempt, kind, detail) "
                   "select job, key, attempt, 'error', 'dead: last row at ' || max(ts) from main.bric_log "
                   "where job = ?1 and key = ?2 and attempt is not null group by attempt "
                   "having sum(kind in ('close', 'error')) = 0 and max(ts) < datetime('now', (-2 * ?3) || ' seconds')",
      brief, key, env("BRIC_TIMEOUT", "120"));
    Attempt a = { t, brief, key };
    a.attempt = q(G.log, NULL, "select 1 + count(*) from main.bric_log where job = ?1 and key = ?2 and kind in ('close', 'error')", brief, key);
    char *err = tools(t, spec);
    int rc = 1;
    if (err) sqlite3_free(logrow(&a, "error", NULL, err));
    else q(G.log, &rc, "insert into main.bric_log (job, key, attempt, turn, kind, detail) values (?1, ?2, ?3, 0, 'open', json(?4))", brief, key, a.attempt, t->tools);
    sqlite3_free(err);
    if (!rc) {
        char *system = sqlite3_mprintf("%s\n\n%s", brief, t->ddl);
        char *messages = q(G.log, NULL, "select json_array(json_object('role', 'user', 'content', ?1))", key);
        int turns = atoi(env("BRIC_TURNS", "40"));
        for (a.turn = 1; a.turn <= turns && !a.done; a.turn++) row = turn(&a, system, &messages);
        if (!a.done) sqlite3_free(logrow(&a, "error", NULL, "turn cap"));
        sqlite3_free(system);
        sqlite3_free(messages);
    }
    for (int i = 0; i < a.n; i++) {
        sqlite3_free(a.seq[i]);
        sqlite3_free(a.text[i]);
    }
    sqlite3_free(a.seq);
    sqlite3_free(a.text);
    sqlite3_free(a.attempt);
    return row;
}

static int open_cursor(sqlite3_vtab *t, sqlite3_vtab_cursor **c)
{
    (void)t;
    *c = sqlite3_malloc(sizeof(Cursor));
    memset(*c, 0, sizeof(Cursor));
    return SQLITE_OK;
}

static int close_cursor(sqlite3_vtab_cursor *c)
{
    sqlite3_finalize(((Cursor *)c)->s);
    sqlite3_free(c);
    return SQLITE_OK;
}

static int next(sqlite3_vtab_cursor *c)
{
    Cursor *cur = (Cursor *)c;
    cur->rc = cur->s ? sqlite3_step(cur->s) : SQLITE_DONE;
    return cur->rc == SQLITE_ROW || cur->rc == SQLITE_DONE ? SQLITE_OK : cur->rc;
}

static int eof(sqlite3_vtab_cursor *c)
{
    return ((Cursor *)c)->rc != SQLITE_ROW;
}

static int column(sqlite3_vtab_cursor *c, sqlite3_context *ctx, int i)
{
    sqlite3_result_value(ctx, sqlite3_column_value(((Cursor *)c)->s, i));
    return SQLITE_OK;
}

static int rowid(sqlite3_vtab_cursor *c, sqlite3_int64 *r)
{
    (void)c;
    *r = 0;
    return SQLITE_OK;
}

static int run_connect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **out, char **err)
{
    (void)aux; (void)argc; (void)argv;
    if (!G.table) {
        *err = sqlite3_mprintf("run needs an insert target");
        return SQLITE_ERROR;
    }
    Table *t = sqlite3_malloc(sizeof *t);
    memset(t, 0, sizeof *t);
    const char *table = G.table, *schema = G.schema;
    char *sql = sqlite3_mprintf("select sql from \"%w\".sqlite_schema where name = ?1", schema);
    t->ddl = q(db, NULL, sql, table);
    sqlite3_free(sql);
    t->del = sqlite3_mprintf("delete from \"%w\".\"%w\" where rowid = ?1", schema, table);
    char *declare = q(db, NULL, "select 'create table x(' || group_concat('\"' || name || '\"') || ', brief hidden, k hidden, tools hidden)' from pragma_table_info(?1, ?2)", table, schema);
    t->columns = atoi(q(db, NULL, "select count(*) from pragma_table_info(?1, ?2)", table, schema));
    t->insert = q(db, NULL, "select 'insert into \"' || ?2 || '\".\"' || ?1 || '\" (\"key\", ' || group_concat('\"' || name || '\"') || ') select ?1, ' || group_concat('json_extract(?2, ''$.\"' || name || '\"'')') from pragma_table_info(?1, ?2) where name != 'key'", table, schema);
    t->read = q(db, NULL, "select 'select json_object(' || group_concat('''' || name || ''', \"' || name || '\"') || ') from \"' || ?2 || '\".\"' || ?1 || '\" where rowid = ?1' from pragma_table_info(?1, ?2)", table, schema);
    t->yield = q(db, NULL, "select 'select ' || group_concat('json_extract(?1, ''$.\"' || name || '\"'')') from pragma_table_info(?1, ?2)", table, schema);
    t->submit = q(db, NULL,
        "select json_object('name', 'submit', 'description', 'insert the row for this key into ' || ?1 || '. sqlite validates it against the ddl in the system prompt; an error is your receipt, so correct and resubmit. a column naming a source takes the number from the [seq N] head of the tool receipt whose own text contains your quote verbatim (not the seq of the navigation, and never a web_search result, which has no seq): read the page with a tool, then cite that receipt', "
        "'input_schema', json_object('type', 'object', "
        "'properties', json_group_object(name, json_object('type', case when type like '%int%' then 'integer' when type like '%rea%' or type like '%flo%' or type like '%dou%' or type like '%num%' then 'number' else 'string' end, "
        "'description', trim(substr(?3, instr(?3, name) + length(name), coalesce(nullif(instr(?3, after), 0), length(?3) + 1) - instr(?3, name) - length(name)), ', ' || char(10, 9) || ')'))), "
        "'required', (select json_group_array(name) from pragma_table_info(?1, ?2) where \"notnull\" and name != 'key'))) "
        "from (select name, type, lead(name) over (order by cid) as after from pragma_table_info(?1, ?2)) where name != 'key'", table, schema, t->ddl);
    int rc = declare ? sqlite3_declare_vtab(db, declare) : SQLITE_ERROR;
    sqlite3_free(declare);
    *out = &t->base;
    return rc;
}

static int run_disconnect(sqlite3_vtab *v)
{
    Table *t = (Table *)v;
    sqlite3_free(t->ddl); sqlite3_free(t->submit); sqlite3_free(t->insert); sqlite3_free(t->read); sqlite3_free(t->del);
    sqlite3_free(t->yield); sqlite3_free(t->spec); sqlite3_free(t->tools); sqlite3_free(t->routes);
    sqlite3_free(t);
    return SQLITE_OK;
}

static int run_best(sqlite3_vtab *v, sqlite3_index_info *info)
{
    Table *t = (Table *)v;
    int seen = 0;
    for (int i = 0; i < info->nConstraint; i++) {
        int arg = info->aConstraint[i].iColumn - t->columns + 1;
        if (!info->aConstraint[i].usable || info->aConstraint[i].op != SQLITE_INDEX_CONSTRAINT_EQ || arg < 1) continue;
        info->aConstraintUsage[i].argvIndex = arg;
        info->aConstraintUsage[i].omit = 1;
        seen |= 1 << arg;
    }
    if ((seen & 6) != 6) return SQLITE_CONSTRAINT;
    info->estimatedCost = 1e9;
    return SQLITE_OK;
}

static int run_filter(sqlite3_vtab_cursor *c, int idx, const char *str, int argc, sqlite3_value **argv)
{
    (void)idx; (void)str;
    Cursor *cur = (Cursor *)c;
    Table *t = (Table *)c->pVtab;
    const char *tools = argc > 2 && sqlite3_value_type(argv[2]) != SQLITE_NULL ? (const char *)sqlite3_value_text(argv[2]) : env("BRIC_TOOLS", NULL);
    G.log = strcmp(G.schema, "temp") ? G.db : G.file;
    char *row = attempt(t, (const char *)sqlite3_value_text(argv[0]), (const char *)sqlite3_value_text(argv[1]), tools);
    sqlite3_finalize(cur->s);
    cur->s = NULL;
    if (row) {
        sqlite3_prepare_v2(G.db, t->yield, -1, &cur->s, NULL);
        sqlite3_bind_text(cur->s, 1, row, -1, sqlite3_free);
    }
    return next(c);
}

static int log_connect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **out, char **err)
{
    (void)aux; (void)argc; (void)argv; (void)err;
    *out = sqlite3_malloc(sizeof **out);
    memset(*out, 0, sizeof **out);
    return sqlite3_declare_vtab(db, "create table x(seq, ts, job, key, attempt, turn, kind, tool, detail, text, usage)");
}

static int log_disconnect(sqlite3_vtab *v)
{
    sqlite3_free(v);
    return SQLITE_OK;
}

static int log_best(sqlite3_vtab *v, sqlite3_index_info *info)
{
    (void)v;
    for (int i = 0; i < info->nConstraint; i++)
        if (info->aConstraint[i].usable && info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ && info->aConstraint[i].iColumn == 0) {
            info->aConstraintUsage[i].argvIndex = 1;
            info->aConstraintUsage[i].omit = 1;
            info->idxNum = 1;
            info->estimatedCost = 1;
            break;
        }
    return SQLITE_OK;
}

static int log_filter(sqlite3_vtab_cursor *c, int idx, const char *str, int argc, sqlite3_value **argv)
{
    (void)str; (void)argc;
    Cursor *cur = (Cursor *)c;
    sqlite3_finalize(cur->s);
    sqlite3_prepare_v2(G.log ? G.log : G.file, idx ? "select seq, ts, job, key, attempt, turn, kind, tool, detail, text, usage from main.bric_log where seq = ?1"
                                                 : "select seq, ts, job, key, attempt, turn, kind, tool, detail, text, usage from main.bric_log order by seq", -1, &cur->s, NULL);
    if (idx) sqlite3_bind_value(cur->s, 1, argv[0]);
    return next(c);
}

static sqlite3_module run_module = { 0, NULL, run_connect, run_best, run_disconnect, run_disconnect, open_cursor, close_cursor, run_filter, next, eof, column, rowid };
static sqlite3_module log_module = { 0, log_connect, log_connect, log_best, log_disconnect, log_disconnect, open_cursor, close_cursor, log_filter, next, eof, column, rowid };

static int authorize(void *u, int op, const char *table, const char *b, const char *schema, const char *trigger)
{
    (void)b; (void)trigger;
    if (op != SQLITE_INSERT || !strcmp(table, "bric_log") || !schema) return SQLITE_OK;
    if (G.table && !strcmp(G.table, table) && !strcmp(G.schema, schema)) return SQLITE_OK;
    sqlite3_free(G.table);
    sqlite3_free(G.schema);
    G.table = sqlite3_mprintf("%s", table);
    G.schema = sqlite3_mprintf("%s", schema);
    sqlite3_create_module(u, "run", &run_module, NULL);
    return SQLITE_OK;
}

static const char *ddl =
    "create table if not exists main.bric_log ("
    " seq integer primary key, ts text not null default (datetime('now')), job text not null, key text, attempt integer, turn integer,"
    " kind text not null, tool text, detail text, text text, usage text) strict;"
    "create unique index if not exists main.bric_claim on bric_log (job, key, attempt, kind) where kind in ('open', 'close', 'error');"
    "create view if not exists main.bric_attempt as"
    " select l.job, l.key, l.attempt, l.turn, l.ts, l.kind, l.tool, l.detail, u.input, u.output, u.cache_read"
    " from bric_log as l join (select job, key, max(seq) as seq, sum(usage ->> 'input') as input, sum(usage ->> 'output') as output, sum(usage ->> 'cache_read') as cache_read"
    " from bric_log group by job, key) as u using (seq);";

int sqlite3_bric_init(sqlite3 *db, char **err, const sqlite3_api_routines *api)
{
    SQLITE_EXTENSION_INIT2(api);
    (void)err;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    G.db = db;
    const char *file = sqlite3_db_filename(db, "main");
    if (file && *file) {
        sqlite3_open(file, &G.file);
        sqlite3_busy_timeout(G.file, 30000);
        sqlite3_exec(G.file, "pragma journal_mode = wal", NULL, NULL, NULL);
    } else {
        G.file = db;
    }
    sqlite3_create_function(G.file, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_exec(G.file, ddl, NULL, NULL, NULL);
    sqlite3_create_module(db, "run", &run_module, NULL);
    sqlite3_create_module(db, "bric_log", &log_module, NULL);
    sqlite3_exec(db, "create virtual table if not exists temp.bric_log using bric_log", NULL, NULL, NULL);
    sqlite3_set_authorizer(db, authorize, db);
    return SQLITE_OK;
}
