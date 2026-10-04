#define _GNU_SOURCE
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <ftw.h>
#include <glob.h>
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <limits.h>
#include "sql.h"
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000 || defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 41)
#define addchdir posix_spawn_file_actions_addchdir
#else
#define addchdir posix_spawn_file_actions_addchdir_np
#endif

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
        unsigned char c = *r;
        int n = c < 0x80 ? 1 : c < 0xc2 ? 0 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : c < 0xf5 ? 4 : 0;
        unsigned lo = c == 0xe0 ? 0xa0 : c == 0xf0 ? 0x90 : 0x80, hi = c == 0xed ? 0x9f : c == 0xf4 ? 0x8f : 0xbf;
        for (int i = 1; i < n; i++, lo = 0x80, hi = 0xbf)
            if ((unsigned char)r[i] < lo || (unsigned char)r[i] > hi) n = 0;
        if (!n) continue;
        if (n == 1 && isspace(c)) {
            space = c == '\n' || space == 2 ? 2 : 1;
            continue;
        }
        if (space && w > s) *w++ = space == 2 ? '\n' : ' ';
        space = 0;
        memmove(w, r, n);
        w += n;
        r += n - 1;
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

static void cites(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(sqlite3_context_db_handle(ctx), sql_cites, -1, &s, NULL)) return sqlite3_result_error(ctx, "cites: no bric_fetch", -1);
    for (int i = 0; i < 2; i++) sqlite3_bind_value(s, i + 1, argv[i]);
    int ok = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    if (ok) return sqlite3_result_int(ctx, 1);
    sqlite3_prepare_v2(sqlite3_context_db_handle(ctx), sql_nearest, -1, &s, NULL);
    for (int i = 0; i < 2; i++) sqlite3_bind_value(s, i + 1, argv[i]);
    sqlite3_step(s);
    const char *near = (const char *)sqlite3_column_text(s, 1);
    const char *url = (const char *)sqlite3_column_text(s, 2);
    char *e = url ? sqlite3_mprintf("source %s is a rowid: cite its url, %s", sqlite3_value_text(argv[0]), url)
        : sqlite3_column_type(s, 0) == SQLITE_NULL
        ? sqlite3_mprintf("source %s is not the url of a page read here: `page URL`, then cite that url", sqlite3_value_text(argv[0]))
        : sqlite3_mprintf("quote not found on %s%s%s", sqlite3_value_text(argv[0]), near ? "; nearest: " : "", near ? near : "");
    sqlite3_finalize(s);
    sqlite3_result_error(ctx, e, -1);
    sqlite3_free(e);
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
    char *first, *attempt, *usage, *row, *tools, *dir, **envp;
    int turn, turns, done, prodded;
} Attempt;

static char *logrow(Attempt *a, const char *kind, const char *tool, const char *detail, const char *text)
{
    char turn[16];
    snprintf(turn, sizeof turn, "%d", a->turn);
    int rc;
    char *seq = q(&rc, sql_log, a->target, a->key, a->attempt, turn, kind, tool, detail, text, a->usage);
    sqlite3_free(a->usage);
    a->usage = NULL;
    a->done |= rc == SQLITE_BUSY || (strcmp(kind, "call") && strcmp(kind, "receipt") && strcmp(kind, "reply"));
    return seq;
}

static char *receipt(Attempt *a, char *text, const char *images)
{
    squeeze(text);
    char *detail = q(NULL, sql_receipt_detail, text, images);
    char *seq = logrow(a, "receipt", "sh", detail, text);
    sqlite3_free(detail);
    if (!seq) {
        sqlite3_free(text);
        text = sqlite3_mprintf("receipt not stored: %s", sqlite3_errmsg(L));
        seq = logrow(a, "receipt", "sh", text, text);
    }
    char *shown = q(NULL, sql_receipt_shown, seq ? seq : "?", text, "20000", images);
    sqlite3_free(seq);
    sqlite3_free(text);
    return shown;
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

static int csv(void *arg, int n, char **v, char **name)
{
    Buf *b = arg;
    for (int pass = !b->n ? 0 : 1; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            const char *s = pass ? v[i] ? v[i] : "" : name[i];
            char *f = strpbrk(s, ",\"\n\r") ? sqlite3_mprintf("%s\"%w\"%s", i ? "," : "", s, i + 1 < n ? "" : "\n")
                                             : sqlite3_mprintf("%s%s%s", i ? "," : "", s, i + 1 < n ? "" : "\n");
            on_body(f, 1, strlen(f), b);
            sqlite3_free(f);
        }
    return 0;
}

static void serve(int c)
{
    Buf req = { sqlite3_mprintf(""), 0 }, res = { sqlite3_mprintf(""), 0 };
    char chunk[65536], *body = NULL, *err = NULL;
    long len = -1;
    ssize_t n;
    while ((!(body = strstr(req.out, "\r\n\r\n")) || (long)(req.n - (body + 4 - req.out)) < len) && (n = read(c, chunk, sizeof chunk)) > 0) {
        on_body(chunk, 1, n, &req);
        char *h = strstr(req.out, "Content-Length:");
        if (h) len = atol(h + 15);
    }
    int rc = body ? sqlite3_exec(L, body + 4, csv, &res, &err) : SQLITE_ERROR, open = !sqlite3_get_autocommit(L);
    if (open) sqlite3_exec(L, "rollback", NULL, NULL, NULL);
    char *head = sqlite3_mprintf("HTTP/1.0 %d OK\r\nContent-Type: text/csv\r\n\r\n%s%s%s", rc || open ? 500 : 200, err || open ? "error: " : "",
                                 err ? err : open ? "transaction left open" : "", open ? "; the transaction is rolled back" : "");
    for (const char *p = head, *e = p + strlen(p); p < e && (n = write(c, p, e - p)) > 0; p += n);
    for (const char *p = res.out, *e = p + res.n; p < e && (n = write(c, p, e - p)) > 0; p += n);
    sqlite3_free(head);
    sqlite3_free(err);
    sqlite3_free(req.out);
    sqlite3_free(res.out);
    close(c);
}

static char *shell(Attempt *a, const char *script, char **images)
{
    char *path = sqlite3_mprintf("%s/.script", a->dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600), out[2];
    if (fd < 0 || write(fd, script, strlen(script)) < 0 || close(fd) || pipe(out)) {
        sqlite3_free(path);
        return sqlite3_mprintf("error: %s", strerror(errno));
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    addchdir(&fa, a->dir);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
#endif
    char *cmd = sqlite3_mprintf("%s", a->shell), *argv[64], *w = cmd;
    int n = 0;
    while (n < 62 && (argv[n] = strsep(&w, " \t"))) if (*argv[n]) n++;
    argv[n++] = path;
    argv[n] = NULL;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s/.db", a->dir);
    unlink(sa.sun_path);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(sock, (struct sockaddr *)&sa, sizeof sa) || listen(sock, 8)) {
        close(sock);
        sock = -1;
    }
    posix_spawn_file_actions_addclose(&fa, sock);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &fa, &at, argv, a->envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(out[1]);
    sqlite3_free(path);
    sqlite3_free(cmd);
    if (rc) {
        close(out[0]);
        close(sock);
        return sqlite3_mprintf("error: %s", strerror(rc));
    }
    Buf b = { sqlite3_mprintf(""), 0 };
    long limit = atol(env("BRIC_TIMEOUT", "120"));
    time_t start = time(NULL);
    const char *cut = NULL;
    for (;;) {
        struct pollfd pf[] = { { out[0], POLLIN, 0 }, { sock, POLLIN, 0 } };
        long left = limit - (time(NULL) - start);
        char chunk[65536];
        int ready = left > 0 ? poll(pf, 2, left * 1000) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            cut = "killed after timeout";
            break;
        }
        if (pf[1].revents) {
            int c = accept(sock, NULL, NULL);
            if (c >= 0) serve(c);
            if (!pf[0].revents) continue;
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
    close(sock);
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    /* whole as well as alone: a png or jpeg cut short by `head -c` passed the
       signature test, and the api rejected the request and ended the attempt */
#define ENDS(t, k) !memcmp(b.out + b.n - (k), t, k)
    const char *mime = cut || b.n < 16 ? NULL
        : !memcmp(b.out, "\x89PNG", 4) && ENDS("IEND\xae\x42\x60\x82", 8) ? "image/png"
        : !memcmp(b.out, "\xff\xd8\xff", 3) && ENDS("\xff\xd9", 2) ? "image/jpeg" : NULL;
#undef ENDS
    char *text;
    if (mime) {
        char *data = base64((unsigned char *)b.out, b.n);
        *images = q(NULL, sql_image, mime, data);
        sqlite3_free(data);
        text = sqlite3_mprintf("[%s, %lld bytes]", mime, (long long)b.n);
    } else if (memchr(b.out, 0, b.n)) {
        text = sqlite3_mprintf("[%lld bytes of binary output: stdout is shown as an image only when it is one png or jpeg, whole and alone]", (long long)b.n);
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
    char *sh = q(NULL, sql_sh_tool, a->target);
    a->tools = q(NULL, sql_push, "[{\"type\":\"web_search_20250305\",\"name\":\"web_search\"}]", sh);
    sqlite3_free(sh);
}

static int settled(Attempt *a)
{
    char *row = q(NULL, a->row, a->key);
    if (row) sqlite3_free(logrow(a, "close", NULL, row, NULL));
    sqlite3_free(row);
    return a->done;
}

static void turn(Attempt *a, const char *system, char **messages)
{
    char *err = NULL;
    char *body = q(NULL, sql_request, env("BRIC_MODEL", ""), system, a->tools, *messages, env("BRIC_PARAMS", "{}"));
    char *reply = infer(body, &err);
    sqlite3_free(body);
    if (!reply) {
        sqlite3_free(logrow(a, "error", NULL, err, NULL));
        sqlite3_free(err);
        return;
    }
    a->usage = q(NULL, sql_usage, reply);
    char *content = q(NULL, sql_field, reply, "$", "content");
    sqlite3_free(logrow(a, "reply", NULL, content, NULL));
    char *next = q(NULL, sql_message, *messages, "assistant", content);
    sqlite3_free(content);
    sqlite3_free(*messages);
    *messages = next;
    char *calls = q(NULL, sql_calls, reply);
    char *count = q(NULL, sql_count, calls);
    int n = atoi(count);
    sqlite3_free(count);
    char *results = sqlite3_mprintf("[]");
    for (int i = 0; i < n; i++) {
        char index[16];
        snprintf(index, sizeof index, "$[%d]", i);
        char *name = q(NULL, sql_field, calls, index, "name");
        char *input = q(NULL, sql_field, calls, index, "input");
        char *id = q(NULL, sql_field, calls, index, "id");
        char *shown, *images = NULL;
        sqlite3_free(logrow(a, "call", name, input, NULL));
        if (!strcmp(name, "sh")) {
            char *script = q(NULL, sql_field, input, "$", "command");
            char *text = script ? shell(a, script, &images)
                : sqlite3_mprintf("error: sh takes {\"command\": \"...\"}, and this call carried %s", input);
            shown = receipt(a, text, images);
            sqlite3_free(script);
            sqlite3_free(images);
        } else {
            shown = sqlite3_mprintf("unknown tool %s", name);
            sqlite3_free(logrow(a, "receipt", name, shown, NULL));
        }
        char *result = q(NULL, sql_result, id, shown);
        char *more = q(NULL, sql_push, results, result);
        sqlite3_free(result);
        sqlite3_free(results);
        results = more;
        sqlite3_free(shown);
        sqlite3_free(name);
        sqlite3_free(input);
        sqlite3_free(id);
    }
    char *stop = q(NULL, sql_field, reply, "$", "stop_reason");
    /* deepseek ends a turn of only server searches with tool_use, not pause_turn */
    int paused = !n && stop && (!strcmp(stop, "pause_turn") || !strcmp(stop, "tool_use"));
    sqlite3_free(stop);
    /* a reply with no call and no row is reminded once, not failed: a small
       model often gives its answer as text first */
    int prod = !n && !paused && !settled(a) && !a->prodded++;
    if (!n && !paused && !prod && !settled(a)) {
        char *text = q(NULL, sql_no_call, reply);
        sqlite3_free(logrow(a, "error", NULL, text, NULL));
        sqlite3_free(text);
    }
    if (!a->done && !paused) {
        char left[16];
        snprintf(left, sizeof left, "%d", a->turns - a->turn);
        char *nudged = prod || a->turns - a->turn <= 3 ? q(NULL, sql_nudge, results, left, a->target) : NULL;
        next = q(NULL, sql_message, *messages, "user", nudged ? nudged : results);
        sqlite3_free(nudged);
        sqlite3_free(*messages);
        *messages = next;
    }
    sqlite3_free(reply);
    sqlite3_free(results);
    sqlite3_free(calls);
}

static void schema(void)
{
    char *old = q(NULL, "select 1 from sqlite_schema where name = 'bric_page_index'");
    if (old) sqlite3_exec(L, "drop view bric_receipt; drop trigger bric_page_index; drop table bric_page", NULL, NULL, NULL);
    sqlite3_free(old);
    sqlite3_exec(L, sql_schema, NULL, NULL, NULL);
    sqlite3_exec(L, "alter table bric_job add column model text", NULL, NULL, NULL);
    sqlite3_exec(L, "alter table bric_job add column params text", NULL, NULL, NULL);
    sqlite3_exec(L, "alter table bric_job add column skills text", NULL, NULL, NULL);
}

/* the shell reads pages an agent was pointed at, so it gets no variable it
   was not given: the worker's environment holds keys and tokens. a few that
   name the user and locale pass, and any a job names in BRIC_PASS */
static int passes(const char *kv)
{
    static const char *keep[] = { "HOME", "USER", "LOGNAME", "LANG", "TZ", "TERM", "TMPDIR", "SHELL", NULL };
    size_t len = strcspn(kv, "=");
    if (!strncmp(kv, "LC_", 3)) return 1;
    for (const char **k = keep; *k; k++)
        if (strlen(*k) == len && !strncmp(kv, *k, len)) return 1;
    for (const char *p = env("BRIC_PASS", ""); *p; p += strcspn(p, " ")) {
        p += strspn(p, " ");
        size_t w = strcspn(p, " ");
        if (w && w == len && !strncmp(kv, p, len)) return 1;
    }
    return 0;
}

static char **childenv(const char *dir)
{
    int n = 0, m = 0;
    while (environ[n]) n++;
    char **e = sqlite3_malloc((n + 3) * sizeof *e);
    for (int i = 0; i < n; i++)
        if (passes(environ[i])) e[m++] = environ[i];
    e[m++] = sqlite3_mprintf("PATH=%s/bin:%s", dir, env("PATH", "/usr/bin:/bin"));
    e[m++] = sqlite3_mprintf("BRIC_DB=%s/.db", dir);
    e[m] = NULL;
    return e;
}

static const char db_script[] = "#!/bin/sh\n# db \"sql\" - run sql against the research database, csv with a header row; the argument or stdin\n"
    "exec curl -s --fail-with-body --unix-socket \"$BRIC_DB\" --data-binary \"${1:-@-}\" http://db/\n";

static const char *cp_from, *cp_to;

static int cp_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)f;
    char *to = sqlite3_mprintf("%s%s", cp_to, p + strlen(cp_from));
    if (flag == FTW_D) mkdir(to, 0700);
    else if (flag == FTW_F) {
        int in = open(p, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 0777);
        char buf[65536];
        for (ssize_t n; (n = read(in, buf, sizeof buf)) > 0 && write(out, buf, n) == n;);
        close(in);
        close(out);
    }
    sqlite3_free(to);
    return 0;
}

static void scratch(const char *dir)
{
    char *bin = sqlite3_mprintf("%s/bin", dir), *sk = sqlite3_mprintf("%s/skills", dir), *db = sqlite3_mprintf("%s/bin/db", dir);
    mkdir(bin, 0700);
    mkdir(sk, 0700);
    FILE *f = fopen(db, "w");
    if (f) {
        fputs(db_script, f);
        fclose(f);
        chmod(db, 0700);
    }
    glob_t g = { 0 };
    if (*env("BRIC_SKILLS", "") && !glob(env("BRIC_SKILLS", ""), GLOB_BRACE, NULL, &g))
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char real[PATH_MAX], *name = strrchr(g.gl_pathv[i], '/');
            if (!realpath(g.gl_pathv[i], real)) continue;
            char *copy = sqlite3_mprintf("%s/%s", sk, name ? name + 1 : g.gl_pathv[i]), *pat = sqlite3_mprintf("%s/scripts/*", copy);
            cp_from = real;
            cp_to = copy;
            nftw(real, cp_cb, 16, FTW_PHYS);
            glob_t h = { 0 };
            if (!glob(pat, 0, NULL, &h))
                for (size_t j = 0; j < h.gl_pathc; j++) {
                    char *to = sqlite3_mprintf("%s/%s", bin, strrchr(h.gl_pathv[j], '/') + 1);
                    symlink(h.gl_pathv[j], to);
                    sqlite3_free(to);
                }
            globfree(&h);
            sqlite3_free(copy);
            sqlite3_free(pat);
        }
    globfree(&g);
    sqlite3_free(bin);
    sqlite3_free(sk);
    sqlite3_free(db);
}

static char *skills(const char *dir)
{
    char *s = sqlite3_mprintf(""), *pat = sqlite3_mprintf("%s/skills/*/SKILL.md", dir);
    glob_t g = { 0 };
    if (!glob(pat, 0, NULL, &g))
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r");
            char line[4096], *name = NULL, *desc = NULL;
            while (f && fgets(line, sizeof line, f)) {
                line[strcspn(line, "\n")] = 0;
                char **at = !strncmp(line, "name:", 5) ? &name : !strncmp(line, "description:", 12) ? &desc : NULL;
                if (at && !*at) *at = sqlite3_mprintf("%s", line + strcspn(line, ":") + 1 + strspn(line + strcspn(line, ":") + 1, " "));
            }
            if (f) fclose(f);
            if (name && desc) {
                char *t = sqlite3_mprintf("%s\n- %s: %s (`cat %s`)", s, name, desc, g.gl_pathv[i] + strlen(dir) + 1);
                sqlite3_free(s);
                s = t;
            }
            sqlite3_free(name);
            sqlite3_free(desc);
        }
    globfree(&g);
    sqlite3_free(pat);
    if (*s) {
        char *t = sqlite3_mprintf("\n\nskills are what has already been worked out for this job."
            " read one with the command beside it before its first use: every command starts in the"
            " working directory, which holds skills/, and a skill's scripts are on your path.%s", s);
        sqlite3_free(s);
        s = t;
    }
    return s;
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
    a.row = q(NULL, sql_row, a.target);
    tools(&a);
    q(NULL, sql_dead, a.target, a.key);
    a.attempt = q(NULL, sql_attempt, a.target, a.key);
    char *closed = q(NULL, a.row, a.key), *source = q(NULL, sql_source, a.target);
    char *row = source ? q(NULL, sql_row, source) : NULL, *given = row ? q(NULL, row, a.key) : NULL;
    a.first = q(NULL, sql_first, a.key, given);
    sqlite3_free(source);
    sqlite3_free(row);
    sqlite3_free(given);
    a.dir = sqlite3_mprintf("%s/bric.XXXXXX", env("TMPDIR", "/tmp"));
    if (!mkdtemp(a.dir)) {
        char *err = sqlite3_mprintf("%s: %s", a.dir, strerror(errno));
        sqlite3_free(logrow(&a, "error", NULL, err, NULL));
        sqlite3_free(err);
        sqlite3_free(a.dir);
        a.dir = NULL;
    } else {
        scratch(a.dir);
        a.envp = childenv(a.dir);
    }
    char *index = skills(a.dir ? a.dir : "");
    char *system = sqlite3_mprintf("%s\n\n%s%s", a.brief, ddl, index);
    sqlite3_free(index);
    int rc = 1;
    char self[16];
    snprintf(self, sizeof self, "%d", getpid());
    if (a.dir && !closed) {
        q(NULL, "begin immediate");
        q(NULL, sql_open, a.target, a.key, a.attempt, system, env("BRIC_WORKERS", "4"), self, a.tools, a.shell, env("BRIC_MODEL", ""), env("BRIC_PARAMS", "{}"), a.first);
        char *opened = q(NULL, "select changes()");
        q(NULL, "commit");
        rc = !atoi(opened ? opened : "0");
        sqlite3_free(opened);
    }
    if (!rc) rc = a.done || settled(&a);
    if (!rc) {
        char *quoted = q(NULL, sql_quote, a.first);
        char *messages = q(NULL, sql_message, "[]", "user", quoted);
        sqlite3_free(quoted);
        a.turns = atoi(env("BRIC_TURNS", "40"));
        for (a.turn = 1; a.turn <= a.turns && !a.done; a.turn++) turn(&a, system, &messages);
        if (!a.done && !settled(&a)) sqlite3_free(logrow(&a, "error", NULL, "turn cap", NULL));
        sqlite3_free(messages);
    }
    sqlite3_free(system);
    char *kind = a.done ? q(NULL, sql_kind, a.target, a.key, a.attempt) : closed ? sqlite3_mprintf("close") : NULL;
    sqlite3_free(closed);
    sqlite3_free(a.first);
    sqlite3_free(a.attempt);
    sqlite3_free(a.row);
    sqlite3_free(a.tools);
    sqlite3_free(ddl);
    if (a.envp) {
        int n = 0;
        while (a.envp[n + 1]) n++;
        sqlite3_free(a.envp[n]);
        sqlite3_free(a.envp[n - 1]);
        sqlite3_free(a.envp);
        nftw(a.dir, unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
    }
    sqlite3_free(a.dir);
    return kind;
}

static void dispatch(void);

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
    dispatch();
}

static void spawn(sqlite3_stmt *job, const char *key)
{
    while (waitpid(-1, NULL, WNOHANG) > 0);
    Dl_info self;
    dladdr((void *)spawn, &self);
    const char *col[7] = { 0 };
    for (int i = 0; job && i < 7; i++) col[i] = (const char *)sqlite3_column_text(job, i);
    char *load = sqlite3_mprintf(".load %s", self.dli_fname);
    char *wait = sqlite3_mprintf("pragma busy_timeout = %d; begin immediate; commit", 1000 * atoi(env("BRIC_TIMEOUT", "120")));
    char *sql = job ? sqlite3_mprintf("select run(%Q, %Q, key, %Q) from \"%w\" where \"key\" = %Q", col[1], col[2], col[3], col[0], key) : NULL;
    if (!fork()) {
        setsid();
        if (job) setenv("BRIC_WORKER", "1", 1);
        if (col[4]) setenv("BRIC_MODEL", col[4], 1);
        if (col[5]) setenv("BRIC_PARAMS", col[5], 1);
        if (col[6]) setenv("BRIC_SKILLS", col[6], 1);
        int null = open("/dev/null", O_RDWR);
        for (int fd = 0; fd < 3; fd++) dup2(null, fd);
        execlp(env("BRIC_SQLITE", "sqlite3"), "sqlite3", file, "-cmd", wait, "-cmd", load, sql ? sql : "select 1", (char *)NULL);
        _exit(1);
    }
    sqlite3_free(load);
    sqlite3_free(wait);
    sqlite3_free(sql);
}

static void dispatch(void)
{
    if (!file) return;
    char *live = q(NULL, sql_live);
    int free = atoi(env("BRIC_WORKERS", "4")) - atoi(live ? live : "0");
    sqlite3_free(live);
    sqlite3_stmt *j, *k;
    if (free <= 0 || sqlite3_prepare_v2(L, sql_jobs, -1, &j, NULL)) return;
    while (free > 0 && sqlite3_step(j) == SQLITE_ROW) {
        char *sql = sqlite3_mprintf(sql_pending, sqlite3_column_text(j, 1), sqlite3_column_text(j, 0), sqlite3_column_text(j, 1), atoi(env("BRIC_ATTEMPTS", "3")), free);
        if (!sqlite3_prepare_v2(L, sql, -1, &k, NULL)) {
            for (; free > 0 && sqlite3_step(k) == SQLITE_ROW; free--) spawn(j, (const char *)sqlite3_column_text(k, 0));
            sqlite3_finalize(k);
        }
        sqlite3_free(sql);
    }
    sqlite3_finalize(j);
}

static int inserted[2];

static void hook(void *arg, int op, const char *dbname, const char *table, sqlite3_int64 rowid)
{
    (void)dbname, (void)rowid;
    int *seen = arg;
    if (op != SQLITE_INSERT || *seen) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(L, sql_job, -1, &s, NULL)) return;
    sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
    *seen = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
}

static int committed(void *arg)
{
    int *seen = arg;
    if (*seen) spawn(NULL, NULL);
    *seen = 0;
    return 0;
}

static void rolled_back(void *arg)
{
    *(int *)arg = 0;
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
        sqlite3_busy_timeout(L, 1000 * atoi(env("BRIC_TIMEOUT", "120")));
        sqlite3_exec(L, "pragma journal_mode = wal; pragma synchronous = normal", NULL, NULL, NULL);
        schema();
        sqlite3 *c[] = { db, L };
        for (int i = 0; i < 2; i++) {
            sqlite3_update_hook(c[i], hook, &inserted[i]);
            sqlite3_commit_hook(c[i], committed, &inserted[i]);
            sqlite3_rollback_hook(c[i], rolled_back, &inserted[i]);
        }
    }
    sqlite3_create_function(L, "alive", 1, SQLITE_UTF8, NULL, alive, NULL, NULL);
    sqlite3_create_function(L, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(db, "squeeze", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, squeeze_fn, NULL, NULL);
    sqlite3_create_function(L, "cites", 2, SQLITE_UTF8 | SQLITE_INNOCUOUS, NULL, cites, NULL, NULL);
    sqlite3_create_function(db, "cites", 2, SQLITE_UTF8 | SQLITE_INNOCUOUS, NULL, cites, NULL, NULL);
    sqlite3_create_function(db, "run", 3, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    sqlite3_create_function(db, "run", 4, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, run, NULL, NULL);
    if (!getenv("BRIC_WORKER")) dispatch();
    return SQLITE_OK;
}
