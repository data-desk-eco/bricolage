#define _GNU_SOURCE
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <poll.h>
#include <spawn.h>
#include <time.h>
#include <sys/wait.h>
#include "sql.h"

#define CAP (4 << 20)

static sqlite3 *L;
static const char *file;
extern char **environ;

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

static void alive(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    int pid = sqlite3_value_int(argv[0]);
    sqlite3_result_int(ctx, pid > 0 && (!kill(pid, 0) || errno == EPERM));
}

static void squeeze_fn(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    char *s = sqlite3_mprintf("%s", sqlite3_value_text(argv[0]) ? (const char *)sqlite3_value_text(argv[0]) : "");
    squeeze(s);
    sqlite3_result_text(ctx, s, -1, sqlite3_free);
}

typedef struct { char *out; size_t n; } Buf;

static size_t on_body(char *p, size_t size, size_t n, void *arg)
{
    Buf *b = arg;
    b->out = sqlite3_realloc64(b->out, b->n + size * n + 1);
    memcpy(b->out + b->n, p, size * n);
    b->n += size * n;
    b->out[b->n] = 0;
    return size * n;
}

static char *infer(const char *body, char **err)
{
    for (int i = 0;; i++) {
        Buf b = { NULL, 0 };
        long status = 0;
        char *key = sqlite3_mprintf("x-api-key: %s", env("BRIC_KEY", ""));
        struct curl_slist *hs = curl_slist_append(NULL, "content-type: application/json");
        hs = curl_slist_append(hs, "anthropic-version: 2023-06-01");
        hs = curl_slist_append(hs, key);
        CURL *h = curl_easy_init();
        curl_easy_setopt(h, CURLOPT_URL, env("BRIC_URL", "https://api.anthropic.com/v1/messages"));
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, atol(env("BRIC_TIMEOUT", "120")));
        CURLcode rc = curl_easy_perform(h);
        if (!rc) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(h);
        curl_slist_free_all(hs);
        sqlite3_free(key);
        if (!rc && status < 400) return b.out ? b.out : sqlite3_mprintf("");
        if ((!rc && status != 429 && status < 500) || i == 3) {
            *err = rc ? sqlite3_mprintf("%s", curl_easy_strerror(rc)) : sqlite3_mprintf("%ld %s", status, b.out);
            sqlite3_free(b.out);
            return NULL;
        }
        sqlite3_free(b.out);
        sleep(2 << i);
    }
}

typedef struct {
    const char *target, *brief, *key, *shell;
    char *attempt, *usage, *insert, *tools, *dir, **envp;
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

static char *base64(const unsigned char *p, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *s = sqlite3_malloc64(4 * ((n + 2) / 3) + 1), *w = s;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        *w++ = t[v >> 18 & 63];
        *w++ = t[v >> 12 & 63];
        *w++ = i + 1 < n ? t[v >> 6 & 63] : '=';
        *w++ = i + 2 < n ? t[v & 63] : '=';
    }
    *w = 0;
    return s;
}

static char *shell(Attempt *a, const char *script, char **images)
{
    char *path = sqlite3_mprintf("%s/bric.XXXXXX", env("TMPDIR", "/tmp"));
    int fd = mkstemp(path), out[2];
    if (fd < 0 || write(fd, script, strlen(script)) < 0 || close(fd) || pipe(out)) {
        sqlite3_free(path);
        return sqlite3_mprintf("error: %s", strerror(errno));
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, path, O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
#endif
    char *cmd = sqlite3_mprintf("cd '%q' && %s", a->dir, a->shell);
    char *argv[] = { "sh", "-c", cmd, NULL };
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "sh", &fa, &at, argv, a->envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(out[1]);
    unlink(path);
    sqlite3_free(path);
    sqlite3_free(cmd);
    if (rc) {
        close(out[0]);
        return sqlite3_mprintf("error: %s", strerror(rc));
    }
    Buf b = { sqlite3_mprintf(""), 0 };
    long limit = atol(env("BRIC_TIMEOUT", "120"));
    time_t start = time(NULL);
    const char *cut = NULL;
    for (;;) {
        struct pollfd pf = { out[0], POLLIN, 0 };
        long left = limit - (time(NULL) - start);
        char chunk[65536];
        int ready = left > 0 ? poll(&pf, 1, left * 1000) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            cut = "killed after timeout";
            break;
        }
        ssize_t n = read(out[0], chunk, sizeof chunk);
        if (n <= 0) break;
        if (b.n + n > CAP) {
            cut = "output cut at 4 MiB";
            break;
        }
        on_body(chunk, 1, n, &b);
    }
    if (cut) {
#ifdef POSIX_SPAWN_SETSID
        kill(-pid, SIGKILL);
#else
        kill(pid, SIGKILL);
#endif
    }
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    const char *mime = b.n > 4 && !memcmp(b.out, "\x89PNG", 4) ? "image/png"
                     : b.n > 3 && !memcmp(b.out, "\xff\xd8\xff", 3) ? "image/jpeg" : NULL;
    char *text;
    if (mime) {
        char *data = base64((unsigned char *)b.out, b.n);
        *images = q(NULL, sql_image, mime, data);
        sqlite3_free(data);
        text = sqlite3_mprintf("[%s, %lld bytes]", mime, (long long)b.n);
    } else if (memchr(b.out, 0, b.n)) {
        text = sqlite3_mprintf("[%lld bytes of binary output]", (long long)b.n);
    } else {
        text = b.out;
        b.out = NULL;
    }
    sqlite3_free(b.out);
    if (cut || code) {
        char *tail = cut ? sqlite3_mprintf("%s\n[%s]", text, cut) : sqlite3_mprintf("%s\n[exit %d]", text, code);
        sqlite3_free(text);
        text = tail;
    }
    return text;
}

static void tools(Attempt *a)
{
    char *sh = q(NULL, sql_sh_tool), *submit = q(NULL, sql_submit_tool, a->target);
    char *with = q(NULL, sql_push, "[{\"type\":\"web_search_20250305\",\"name\":\"web_search\"}]", sh);
    a->tools = q(NULL, sql_push, with, submit);
    sqlite3_free(sh);
    sqlite3_free(submit);
    sqlite3_free(with);
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
    char *results = sqlite3_mprintf("[]");
    for (int i = 0; i < n && !a->done; i++) {
        char index[16];
        snprintf(index, sizeof index, "$[%d]", i);
        char *name = q(NULL, sql_field, calls, index, "name");
        char *input = q(NULL, sql_field, calls, index, "input");
        char *id = q(NULL, sql_field, calls, index, "id");
        char *shown, *images = NULL;
        sqlite3_free(logrow(a, "call", name, input));
        if (!strcmp(name, "submit")) {
            shown = submit(a, input);
            if (!shown) sqlite3_free(logrow(a, "close", NULL, input));
            else sqlite3_free(logrow(a, "receipt", name, shown));
        } else if (!strcmp(name, "sh")) {
            char *script = q(NULL, sql_field, input, "$", "script");
            char *text = shell(a, script ? script : "", &images);
            shown = receipt(a, name, text, images);
            sqlite3_free(script);
            sqlite3_free(images);
        } else {
            shown = sqlite3_mprintf("unknown tool %s", name);
            sqlite3_free(logrow(a, "receipt", name, shown));
        }
        if (shown) {
            char *result = q(NULL, sql_result, id, shown);
            char *more = q(NULL, sql_push, results, result);
            sqlite3_free(result);
            sqlite3_free(results);
            results = more;
        }
        sqlite3_free(shown);
        sqlite3_free(name);
        sqlite3_free(input);
        sqlite3_free(id);
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

static char **childenv(void)
{
    int n = 0, m = 0;
    while (environ[n]) n++;
    char **e = sqlite3_malloc((n + 2) * sizeof *e);
    for (int i = 0; i < n; i++)
        if (strncmp(environ[i], "BRIC_", 5)) e[m++] = environ[i];
    e[m++] = sqlite3_mprintf("BRIC_DB=%s", file);
    e[m] = NULL;
    return e;
}

static int unlink_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

static char *attempt(const char *target, const char *brief, const char *key, const char *shell)
{
    Attempt a = { target, brief, key, shell };
    schema();
    char *ddl = q(NULL, sql_ddl, a.target);
    if (!ddl) return NULL;
    a.insert = q(NULL, sql_insert, a.target);
    q(NULL, sql_dead, a.brief, a.key);
    a.attempt = q(NULL, sql_attempt, a.brief, a.key);
    char *closed = q(NULL, sql_closed, a.brief, a.key);
    char *system = sqlite3_mprintf("%s\n\n%s", a.brief, ddl);
    int rc = 1, taken = 0;
    char self[16];
    snprintf(self, sizeof self, "%d", getpid());
    while (!closed && !taken) {
        q(NULL, "begin immediate");
        q(NULL, sql_open, a.brief, a.key, a.attempt, system, env("BRIC_WORKERS", "4"), self);
        char *opened = q(NULL, "select changes()");
        q(NULL, "commit");
        rc = !atoi(opened);
        sqlite3_free(opened);
        if (!rc) break;
        char *t = q(NULL, sql_opened, a.brief, a.key, a.attempt);
        taken = t != NULL;
        sqlite3_free(t);
        if (!taken) sleep(1);
    }
    if (!rc) {
        a.dir = sqlite3_mprintf("%s/bric.XXXXXX", env("TMPDIR", "/tmp"));
        if (!mkdtemp(a.dir)) {
            char *err = sqlite3_mprintf("%s: %s", a.dir, strerror(errno));
            sqlite3_free(logrow(&a, "error", NULL, err));
            sqlite3_free(err);
        } else {
            a.envp = childenv();
            tools(&a);
            q(NULL, sql_open_tools, a.brief, a.key, a.attempt, a.tools, a.shell);
        }
        rc = a.done;
    }
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
    char *kind = a.done ? q(NULL, sql_kind, a.brief, a.key, a.attempt) : closed ? sqlite3_mprintf("close") : NULL;
    sqlite3_free(closed);
    sqlite3_free(a.attempt);
    sqlite3_free(a.insert);
    sqlite3_free(a.tools);
    sqlite3_free(ddl);
    if (a.envp) {
        int n = 0;
        while (a.envp[n + 1]) n++;
        sqlite3_free(a.envp[n]);
        sqlite3_free(a.envp);
        nftw(a.dir, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
    }
    sqlite3_free(a.dir);
    return kind;
}

static const char *shell_arg(int argc, sqlite3_value **argv)
{
    return argc > 3 && sqlite3_value_type(argv[3]) != SQLITE_NULL ? (const char *)sqlite3_value_text(argv[3]) : env("BRIC_SHELL", "sh");
}

static void run(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    const char *target = (const char *)sqlite3_value_text(argv[0]);
    char *ddl = q(NULL, sql_ddl, target);
    if (!ddl) return sqlite3_result_error(ctx, "run: no such table", -1);
    sqlite3_free(ddl);
    char *kind = attempt(target, (const char *)sqlite3_value_text(argv[1]), (const char *)sqlite3_value_text(argv[2]), shell_arg(argc, argv));
    if (kind) sqlite3_result_text(ctx, kind, -1, sqlite3_free);
}

static void spawn(const char *target, const char *brief, const char *source, sqlite3_int64 rowid, const char *shell)
{
    while (waitpid(-1, NULL, WNOHANG) > 0);
    Dl_info self;
    dladdr((void *)spawn, &self);
    char *load = sqlite3_mprintf(".load %s", self.dli_fname);
    char *sql = sqlite3_mprintf("select run(%Q, %Q, key, %Q) from \"%w\" where rowid = %lld", target, brief, shell, source, rowid);
    if (!fork()) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        for (int fd = 0; fd < 3; fd++) dup2(null, fd);
        execlp(env("BRIC_SQLITE", "sqlite3"), "sqlite3", file, "-cmd", load, sql, (char *)NULL);
        _exit(1);
    }
    sqlite3_free(load);
    sqlite3_free(sql);
}

static void hook(void *arg, int op, const char *dbname, const char *table, sqlite3_int64 rowid)
{
    (void)arg, (void)dbname;
    if (op != SQLITE_INSERT) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(L, sql_job, -1, &s, NULL)) return;
    sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW)
        spawn((const char *)sqlite3_column_text(s, 0), (const char *)sqlite3_column_text(s, 1), table, rowid, (const char *)sqlite3_column_text(s, 2));
    sqlite3_finalize(s);
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
        schema();
        sqlite3_update_hook(db, hook, NULL);
    }
    sqlite3_create_function(L, "alive", 1, SQLITE_UTF8, NULL, alive, NULL, NULL);
    sqlite3_create_function(L, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "run", 3, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    sqlite3_create_function(db, "run", 4, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    return SQLITE_OK;
}
