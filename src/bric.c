#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#include <curl/curl.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include "sql.h"

static sqlite3 *L;
static const char *file;
static int children;

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

extern char **environ;

static pid_t browser(char **url)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t n = sizeof a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    bind(s, (struct sockaddr *)&a, n);
    getsockname(s, (struct sockaddr *)&a, &n);
    close(s);
    char *cmd = sqlite3_mprintf("exec %s --port %d >/dev/null 2>&1", env("BRIC_BROWSER", "obscura mcp --http"), ntohs(a.sin_port));
    char *argv[] = { "sh", "-c", cmd, NULL };
    pid_t pid = 0;
    posix_spawnp(&pid, "sh", NULL, NULL, argv, environ);
    sqlite3_free(cmd);
    *url = sqlite3_mprintf("http://127.0.0.1:%d/mcp", ntohs(a.sin_port));
    for (int i = 0; i < 500; i++) {
        Req r = { *url, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"tools/list\"}", 0 };
        http(&r, 1);
        int up = !r.err;
        sqlite3_free(r.out);
        sqlite3_free(r.err);
        if (up) break;
        usleep(10000);
    }
    return pid;
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
    int turn, done;
} Attempt;

static char *logrow(Attempt *a, const char *kind, const char *tool, const char *detail)
{
    char turn[16];
    snprintf(turn, sizeof turn, "%d", a->turn);
    char *seq = q(NULL, sql_log, a->brief, a->key, a->attempt, turn, kind, tool, detail, a->usage);
    sqlite3_free(a->usage);
    a->usage = NULL;
    a->done |= strcmp(kind, "call") && strcmp(kind, "receipt") && strcmp(kind, "reply");
    return seq;
}

static char *receipt(Attempt *a, const char *tool, char *text, const char *images)
{
    squeeze(text);
    char *detail = q(NULL, sql_receipt_detail, text, images);
    char *seq = logrow(a, "receipt", tool, detail);
    sqlite3_free(detail);
    q(NULL, sql_text, seq, text);
    char *shown = q(NULL, sql_receipt_shown, seq, text, "20000", images);
    sqlite3_free(seq);
    sqlite3_free(text);
    return shown;
}

static char *submit(Attempt *a, const char *input)
{
    int rc;
    q(NULL, "savepoint bric");
    q(&rc, a->insert, a->key, input);
    char *err = rc ? sqlite3_mprintf("%s", sqlite3_errmsg(L)) : NULL;
    if (rc) q(NULL, "rollback to bric");
    q(NULL, "release bric");
    return err;
}

static char *tools(Attempt *a, const char *given)
{
    static char *cached_spec, *cached_tools, *cached_routes;
    char *spec = q(NULL, sql_spec, given);
    if (!cached_spec || !spec || strcmp(cached_spec, spec)) {
        sqlite3_free(cached_spec);
        sqlite3_free(cached_tools);
        sqlite3_free(cached_routes);
        cached_spec = spec ? sqlite3_mprintf("%s", spec) : NULL;
        cached_tools = sqlite3_mprintf("[{\"type\":\"web_search_20250305\",\"name\":\"web_search\"}]");
        cached_routes = sqlite3_mprintf("{}");
        char *count = q(NULL, sql_count, spec);
        for (int i = 0; count && i < atoi(count); i++) {
            char index[16];
            snprintf(index, sizeof index, "%d", i);
            char *url = q(NULL, sql_spec_url, spec, index);
            char *names = q(NULL, sql_spec_names, spec, index);
            Req r = { url, "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"tools/list\"}", 0 };
            http(&r, 1);
            char *listed = unsse(r.out);
            if (r.err || r.status >= 400) {
                char *err = sqlite3_mprintf("%s: %s", url, r.err ? r.err : listed);
                sqlite3_free(cached_spec);
                sqlite3_free(spec);
                cached_spec = NULL;
                return err;
            }
            char *found = q(NULL, sql_listed, listed, names);
            char *merged = q(NULL, sql_concat, cached_tools, found);
            char *routes = q(NULL, sql_routes, cached_routes, found, url);
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
    char *submit = q(NULL, sql_submit_tool, a->target);
    a->tools = q(NULL, sql_push, cached_tools, submit);
    a->routes = cached_routes;
    sqlite3_free(submit);
    sqlite3_free(spec);
    return NULL;
}

static void turn(Attempt *a, const char *system, char **messages)
{
    char *err = NULL;
    char *body = q(NULL, sql_request, env("BRIC_MODEL", ""), system, a->tools, *messages);
    char *reply = infer(body, &err);
    sqlite3_free(body);
    if (!reply) {
        sqlite3_free(logrow(a, "error", NULL, err));
        sqlite3_free(err);
        return;
    }
    a->usage = q(NULL, sql_usage, reply);
    char *content = q(NULL, sql_field, reply, "$", "content");
    sqlite3_free(logrow(a, "reply", NULL, content));
    char *next = q(NULL, sql_message, *messages, "assistant", content);
    sqlite3_free(content);
    sqlite3_free(*messages);
    *messages = next;
    char *calls = q(NULL, sql_calls, reply);
    char *count = q(NULL, sql_count, calls);
    int n = atoi(count);
    sqlite3_free(count);
    if (!n) {
        char *text = q(NULL, sql_no_call, reply);
        sqlite3_free(logrow(a, "error", NULL, text));
        sqlite3_free(text);
    }
    sqlite3_free(reply);
    Req r[n];
    char *name[n], *input[n], *id[n], *url[n];
    for (int i = 0; i < n; i++) {
        char index[16];
        snprintf(index, sizeof index, "$[%d]", i);
        name[i] = q(NULL, sql_field, calls, index, "name");
        input[i] = q(NULL, sql_field, calls, index, "input");
        id[i] = q(NULL, sql_field, calls, index, "id");
        url[i] = q(NULL, sql_route, a->routes, name[i]);
        sqlite3_free(logrow(a, "call", name[i], input[i]));
        r[i].url = url[i] ? url[i] : "";
        r[i].body = url[i] ? q(NULL, sql_mcp_call, name[i], input[i]) : "";
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
                 : q(NULL, sql_receipt_text, json, status);
            char *images = q(NULL, sql_receipt_images, json);
            shown = receipt(a, name[i], text, images);
            sqlite3_free(images);
            text = NULL;
        }
        if (text) sqlite3_free(logrow(a, "receipt", name[i], text));
        char *result = q(NULL, sql_result, id[i], shown);
        char *more = q(NULL, sql_push, results, result);
        sqlite3_free(result);
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
        next = q(NULL, sql_message, *messages, "user", results);
        sqlite3_free(*messages);
        *messages = next;
    }
    sqlite3_free(results);
    sqlite3_free(calls);
}

static void schema(void)
{
    sqlite3_exec(L, sql_schema, NULL, NULL, NULL);
}

static char *attempt(const char *target, const char *brief, const char *key, const char *spec)
{
    Attempt a = { target, brief, key };
    schema();
    char *ddl = q(NULL, sql_ddl, a.target);
    if (!ddl) return NULL;
    a.insert = q(NULL, sql_insert, a.target);
    q(NULL, sql_dead, a.brief, a.key, env("BRIC_TIMEOUT", "120"));
    a.attempt = q(NULL, sql_attempt, a.brief, a.key);
    char *closed = q(NULL, sql_closed, a.brief, a.key), *own = NULL, *err = NULL;
    pid_t pid = 0;
    if (!closed) {
        if (!spec) pid = browser(&own);
        err = tools(&a, own ? own : spec);
    }
    int rc = 1;
    char *system = sqlite3_mprintf("%s\n\n%s", a.brief, ddl);
    if (err) sqlite3_free(logrow(&a, "error", NULL, err));
    else if (!a.tools) closed = sqlite3_mprintf("close");
    else q(&rc, sql_open, a.brief, a.key, a.attempt, system, a.tools);
    sqlite3_free(err);
    if (!rc) {
        char *quoted = q(NULL, sql_quote, a.key);
        char *messages = q(NULL, sql_message, "[]", "user", quoted);
        sqlite3_free(quoted);
        int turns = atoi(env("BRIC_TURNS", "40"));
        for (a.turn = 1; a.turn <= turns && !a.done; a.turn++) turn(&a, system, &messages);
        if (!a.done) sqlite3_free(logrow(&a, "error", NULL, "turn cap"));
        sqlite3_free(messages);
    }
    sqlite3_free(system);
    char *kind = a.done ? q(NULL, sql_kind, a.brief, a.key, a.attempt) : closed;
    sqlite3_free(a.attempt);
    sqlite3_free(a.insert);
    if (pid) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
    sqlite3_free(own);
    sqlite3_free(a.tools);
    sqlite3_free(ddl);
    return kind;
}

static const char *spec_arg(int argc, sqlite3_value **argv)
{
    return argc > 3 && sqlite3_value_type(argv[3]) != SQLITE_NULL ? (const char *)sqlite3_value_text(argv[3]) : env("BRIC_TOOLS", NULL);
}

static void run(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    const char *target = (const char *)sqlite3_value_text(argv[0]);
    char *ddl = q(NULL, sql_ddl, target);
    if (!ddl) return sqlite3_result_error(ctx, "run: no such table", -1);
    sqlite3_free(ddl);
    char *kind = attempt(target, (const char *)sqlite3_value_text(argv[1]), (const char *)sqlite3_value_text(argv[2]), spec_arg(argc, argv));
    if (kind) sqlite3_result_text(ctx, kind, -1, sqlite3_free);
}

static void drain(const char *target, const char *brief, const char *source, const char *spec)
{
    char *sql = sqlite3_mprintf(sql_pending, source, target, source);
    int progress;
    do {
        sqlite3_stmt *s;
        progress = 0;
        sqlite3_prepare_v2(L, sql, -1, &s, NULL);
        sqlite3_bind_text(s, 1, brief, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, env("BRIC_TRIES", "3"), -1, SQLITE_STATIC);
        while (sqlite3_step(s) == SQLITE_ROW) {
            char *key = sqlite3_mprintf("%s", sqlite3_column_text(s, 0));
            char *kind = attempt(target, brief, key, spec);
            progress |= kind != NULL;
            sqlite3_free(kind);
            sqlite3_free(key);
        }
        sqlite3_finalize(s);
    } while (progress);
    sqlite3_free(sql);
}

static void drain_fn(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)ctx;
    q(NULL, "begin immediate");
    q(NULL, "commit");
    drain((const char *)sqlite3_value_text(argv[0]), (const char *)sqlite3_value_text(argv[1]), (const char *)sqlite3_value_text(argv[2]), spec_arg(argc, argv));
}

static void spawn(const char *target, const char *brief, const char *source, const char *spec)
{
    while (waitpid(-1, NULL, WNOHANG) > 0) children--;
    char *live = q(NULL, sql_live, brief, env("BRIC_TIMEOUT", "120"));
    int busy = atoi(live) > children ? atoi(live) : children;
    sqlite3_free(live);
    if (busy >= atoi(env("BRIC_WORKERS", "4"))) return;
    Dl_info self;
    dladdr((void *)spawn, &self);
    char *load = sqlite3_mprintf(".load %s", self.dli_fname);
    char *sql = sqlite3_mprintf("select drain(%Q, %Q, %Q, %Q)", target, brief, source, spec);
    pid_t pid = fork();
    if (!pid) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        for (int fd = 0; fd < 3; fd++) dup2(null, fd);
        execlp(env("BRIC_SQLITE", "sqlite3"), "sqlite3", file, "-cmd", load, sql, (char *)NULL);
        _exit(1);
    }
    children += pid > 0;
    sqlite3_free(load);
    sqlite3_free(sql);
}

static void hook(void *arg, int op, const char *dbname, const char *table, sqlite3_int64 rowid)
{
    (void)arg, (void)dbname, (void)rowid;
    if (op != SQLITE_INSERT) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(L, sql_job, -1, &s, NULL)) return;
    sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, env("BRIC_TOOLS", NULL), -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW)
        spawn((const char *)sqlite3_column_text(s, 0), (const char *)sqlite3_column_text(s, 1), table, (const char *)sqlite3_column_text(s, 2));
    sqlite3_finalize(s);
}

static void job(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    const char *source = (const char *)sqlite3_value_text(argv[0]), *target = (const char *)sqlite3_value_text(argv[1]);
    const char *brief = (const char *)sqlite3_value_text(argv[2]), *spec = argc > 3 ? (const char *)sqlite3_value_text(argv[3]) : NULL;
    if (!file) return sqlite3_result_error(ctx, "job: database is not a file", -1);
    schema();
    q(NULL, sql_job_set, source, target, brief, spec);
    spawn(target, brief, source, spec ? spec : env("BRIC_TOOLS", NULL));
}

int sqlite3_bric_init(sqlite3 *db, char **err, const sqlite3_api_routines *api)
{
    SQLITE_EXTENSION_INIT2(api);
    (void)err;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    file = sqlite3_db_filename(db, "main");
    file = file && *file ? sqlite3_mprintf("%s", file) : NULL;
    L = db;
    if (file && sqlite3_open(file, &L) == SQLITE_OK) {
        sqlite3_busy_timeout(L, 30000);
        sqlite3_exec(L, "pragma journal_mode = wal", NULL, NULL, NULL);
    }
    sqlite3_create_function(L, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "run", 3, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    sqlite3_create_function(db, "run", 4, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    sqlite3_create_function(db, "drain", 3, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, drain_fn, NULL, NULL);
    sqlite3_create_function(db, "drain", 4, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, drain_fn, NULL, NULL);
    sqlite3_create_function(db, "job", 3, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, job, NULL, NULL);
    sqlite3_create_function(db, "job", 4, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, job, NULL, NULL);
    sqlite3_update_hook(db, hook, NULL);
    return SQLITE_OK;
}
