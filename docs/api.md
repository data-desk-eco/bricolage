# API

Loading the extension adds three functions, three tables and two views.
The tables are created on `.load`, so a script can insert into `bric_job`
straight after loading.

## `run(target, brief, key, shell?)`

Runs one research attempt for `key` and inserts the answer into `target`.
`target` names a table you have created; its DDL, with any `CHECK`
constraints and triggers, is the contract the model writes against and the
one SQLite enforces. `brief` is the task in prose, shared by every key in
the job. `shell` is the executor, default `BRIC_SHELL`. The model
has `web_search` and `sh`, and answers by inserting its row with `sqlite3`;
after every turn the worker checks `target` for a row with this `key`, and
the first one it finds closes the attempt. See [Shell](#shell) for what
`sh` is given.

Returns `'close'` when the row was inserted, `'error'` when the attempt
failed, with the `error` row saying why (the model stopped without a row,
the API refused a request, the scratch directory could not be made, or the
turn cap was hit), and `NULL` when another worker claimed the attempt. A key that already has a close row returns `'close'` without a model
call, and an attempt whose worker process is gone (`kill -0` on the pid in
its `open` row) is treated as dead and retried, so a `SELECT run(...)` over
the whole to-do list is safe to rerun.
It waits for one of `BRIC_WORKERS` slots before starting. Only
callable at the top level of a statement, not from views or triggers.

## `alive(pid)`

True when a process with that pid exists. The liveness test behind the
dead-attempt check and the worker slots.

## `squeeze(text)`

Collapses runs of whitespace to one space and strips base64 data URLs and
bytes that are not UTF-8, which an API would refuse. Every receipt is stored
squeezed. A cite trigger should not need it: a phrase
query against `bric_page`, as in `company.sql`, ignores whitespace, case
and punctuation and works from any `sqlite3`, including the model's.

## `bric_job`

One row per job: `source`, the to-do table (any table with a `key` column,
and the primary key here), `target`, `brief` and optional `shell`, `model`, `params` and `skills`.
Insert a row to register a job:

    insert or replace into bric_job (source, target, brief)
    values ('company', 'company_parent', 'Resolve each operator ...');

From then on a key in `source` with no answer is pending, and pending
keys get workers, `sqlite3 db "select run(target, brief, key, shell)"`,
that outlive whatever started them. Work is found by state, not by event:
the extension looks for pending keys when it is loaded, when a row is
inserted into `source` on a connection that has it loaded, and when any
worker finishes. So a trigger on one job's target that inserts into
another's source chains the two jobs, the model's own inserts included,
and after a crash or reboot `sqlite3 db ".load bric"` restarts whatever
was left; nothing needs to run in between. A worker spawned by the hook
waits for the inserting transaction to commit, and does nothing if it
rolls back, so a bulk `.import` loses no keys.

A key is pending when it has no `close` row, no live attempt (an `open`
row whose pid is alive), and fewer than `BRIC_ATTEMPTS` `error` rows; a
key that has failed that many times stays put, and `select key from
source except select key from target` lists them. Delete its error rows
to try again.

`shell` NULL means `BRIC_SHELL`; `model` NULL means `BRIC_MODEL`, so a
cheap model can run one job and a strong one another against the same
database. `params` is a JSON object patched over the request body
(`json_patch`), for whatever the endpoint takes beyond the model name:
`'{"thinking": {"type": "disabled"}}'` for deepseek,
`'{"output_config": {"effort": "low"}}'` or a `max_tokens` for Anthropic.
NULL means `BRIC_PARAMS`. `skills` is a glob of skill directories in the
[Agent Skills](https://agentskills.io) layout, each `<skill>/SKILL.md` with
`name:` and `description:` frontmatter and whatever scripts and references
sit beside it: `./skills/*` takes every skill, `./skills/{archive,web}` a
job's own set, relative to the worker's directory; NULL means no skills.
The system prompt ends with an index, one line per skill from its
frontmatter with the file's path, and the model reads a skill's body with
`cat` when it needs it, so a skill is a receipt like a page. Each skill's
`scripts/` is copied into the scratch directory with the rest of the
skill and linked into `bin`, so a script is a
command and the model need never know where it lives. `skills/` here holds the ones the ch4id job uses. Update or delete the row to change or stop the
job; the change applies to the next worker, not to workers already
running.
The database must be a file: on an in-memory database nothing is
spawned.

## `bric_log`

The append-only log, one row per event. `job` is the brief, `key` and
`attempt` identify the attempt, `turn` counts model calls within it.

| `kind`    | what                                                       |
|-----------|------------------------------------------------------------|
| `open`    | attempt claimed; `detail` is the system prompt, the tools as sent, the `shell` and the worker `pid` |
| `reply`   | a model turn; `detail` is its content verbatim, including any thinking; a `pause_turn` reply (the server paused a long search turn) is resent as is, and only a turn that ends without a call or a row is an error |
| `call`    | a tool call the model made; `tool` and `detail` (arguments; for `sh`, `{"command": ...}`) |
| `receipt` | a tool result; `text` is its squeezed content, `seq` is what a result row cites as its source |
| `close`   | a row for the key exists in the target; `detail` is that row as JSON |
| `error`   | the attempt failed; `detail` says why                       |

`usage` holds the token counts per turn as JSON. A partial unique index
over `(job, key, attempt)` for `open`, `close` and `error` is the claim:
two workers cannot open the same attempt. Triggers refuse every update and
delete, so the log is append-only for the model, which can reach it from
its shell, and for you; drop `bric_log_update` and `bric_log_delete` to
prune.

## `bric_page`

FTS5 over `bric_log.text`, filled by trigger as receipts are stored.
`select rowid, snippet(bric_page, 0, '', '', ' ... ', 48) from bric_page
where bric_page match 'x'` finds every page any attempt has read, and the
rowid is a `seq` a result row may cite. The model runs this through
`db` in its shell; so can you. It is also the cite check: a result
table's trigger asks whether the quote is a phrase on the cited receipt,

    where not exists (
      select 1 from bric_page('"' || replace(new.quote, '"', '""') || '"')
      where rowid = new.source
    )

using the table-valued form, which the `sqlite3` shell allows inside a
trigger where the `match` operator is refused as unsafe.

## `bric_attempt`

One row per `(job, key)`, the progress and cost board, in the order a
glance wants: `key`, `turn`, `age` in seconds since the last log row,
its `kind`, then `calls`, `images` sent, `read` (input plus cache-read
tokens, what the model has been shown) and `output` over the key's whole
log, then `job`, `attempt`, `ts`, `tool`, `detail` and the raw `input`
and `cache_read`. A key whose `age` grows with `kind` still `call` or
`receipt` has a worker stuck in a shell call.

    watch sqlite3 -box db "select * from bric_attempt"

To tail the log itself,
`seq` is monotonic:

    last=0; while :; do sqlite3 db "select seq, key, turn, kind, substr(detail, 1, 100) from bric_log where seq > $last"; last=$(sqlite3 db "select max(seq) from bric_log"); sleep 5; done

## `bric_transcript`

One row per `(job, key, attempt)` with `messages`, the conversation as the
API saw it, rebuilt from the log: the key, each `reply`, and each turn's
receipts as `tool_result` blocks. Together with the `open` row's system
prompt and tools this is the whole request, so any attempt can be replayed
or resumed. Images are not kept.

## Shell

Each `sh` call is one process: `$BRIC_SHELL` split on whitespace, with
the script's path appended, run with the attempt's scratch directory as
its working directory; stdout and stderr merged into the receipt, and
`[exit N]` appended when the status is not zero. The scratch directory is
made under `TMPDIR` when the attempt opens and removed when it ends. It
holds `bin/db` and a symlink to every skill script, so `PATH` starts with
`bin`; `skills/<name>`, a copy of each skill directory, which is the
path the system prompt's index gives; `.script`, the current call; and
`.db`, a unix socket bric listens on for as long as the script runs. `db
"sql"` (or `db` with the sql on stdin) posts it there with curl and
prints csv with a header row, or the error and exit 22. The sql runs on
the worker's own connection, with its busy timeout, so a sandbox needs
nothing but the scratch directory writable and the system's tools
readable; the home directory can be denied outright. The environment is the
worker's minus every `BRIC_*` variable, so the API key is not in the
sandbox, plus `BRIC_DB`, the socket's path. Output that begins with
a PNG or JPEG header is sent to the model as an image, and stays in the
conversation like any receipt: nothing sent is ever rewritten, so the
prompt cache holds. Output with a NUL byte in it, or an image the cut below
truncated, is reported by size only. After `BRIC_TIMEOUT` seconds, or 4
MiB of output, the process group is killed and the receipt says so.
Anything that escapes the process group, such as a container the client
was detached from, is the executor's to stop. Calls in one model
turn run one after another, so files written by the first are there for
the second. `sandbox/` holds an executor for seatbelt, bwrap and docker.

## Configuration

Everything is an environment variable, read when `run` is called:

| variable            | default                                  | what                                              |
|---------------------|------------------------------------------|---------------------------------------------------|
| `BRIC_KEY`          |                                          | sent as `x-api-key`                               |
| `BRIC_MODEL`        |                                          | model name; `bric_job.model` overrides it         |
| `BRIC_PARAMS`       | `{}`                                     | JSON patched over every request body; `bric_job.params` overrides it |
| `BRIC_URL`          | `https://api.anthropic.com/v1/messages`  | any Anthropic-format messages endpoint            |
| `BRIC_SHELL`        | `sh`                                     | the executor each script's path is passed to; `run`'s fourth argument and `bric_job.shell` override it |
| `BRIC_ATTEMPTS`     | `3`                                      | attempts per key before it stops being pending    |
| `BRIC_TURNS`        | `40`                                     | turns per attempt; the last three carry a note telling the model to insert now |
| `BRIC_WORKERS`      | `4`                                      | attempts live at once; `run` waits for a slot     |
| `BRIC_SQLITE`       | `sqlite3`                                | the shell workers run in; must be able to `.load` |
| `BRIC_TIMEOUT`      | `120`                                    | seconds per HTTP call and per shell call           |
