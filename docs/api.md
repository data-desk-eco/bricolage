# API

Loading the extension adds three functions, three tables and two views.
The tables are created on `.load`, so a script can insert into `bric_job`
straight after loading.

## `run(target, brief, key, tools?)`

Runs one research attempt for `key` and inserts the answer into `target`.
`target` names a table you have created; its DDL, with any `CHECK`
constraints and triggers, is the contract the model writes against and the
one SQLite enforces. `brief` is the task in prose, shared by every key in
the job. `tools` is an optional MCP spec in the same form as `BRIC_TOOLS`. Besides
the servers' own tools the model always has `web_search`, `submit` and
`pages`, a full-text search over every receipt in `bric_log`, so an attempt
can quote a page any earlier attempt read.

Returns `'close'` when the row was inserted, `'error'` when the model gave
up, and `NULL` when the attempt did not finish: the key was claimed by
another worker, the tool server could not be reached, or the turn cap was
hit. A key that already has a close row returns `'close'` without a model
call, and an attempt whose worker process is gone (`kill -0` on the pid in
its `open` row) is treated as dead and retried, so a `SELECT run(...)` over
the whole to-do list is safe to rerun.
It waits for one of `BRIC_WORKERS` slots before starting a browser. Only
callable at the top level of a statement, not from views or triggers.

## `alive(pid)`

True when a process with that pid exists. The liveness test behind the
dead-attempt check and the worker slots.

## `squeeze(text)`

Collapses runs of whitespace to one space and strips base64 data URLs. Every
receipt is stored squeezed, so apply it to a quote before matching it
against `bric_log.text`, as the trigger in `company.sql` does.

## `bric_job`

One row per job: `source`, the to-do table (any table with a `key` column,
and the primary key here), `target`, `brief` and an optional `tools` spec.
Insert a row to register a job:

    insert or replace into bric_job (source, target, brief)
    values ('company', 'company_parent', 'Resolve each operator ...');

From then on, on any connection with the extension loaded, each row
inserted into `source` gets a worker, `sqlite3 db "select run(target,
brief, key, tools)"`, that outlives the connection. `tools` NULL means
`BRIC_TOOLS`. Update or delete the row to change or stop the job; the
change applies to the next insert, not to workers already running.
The database must be a file: on an in-memory database nothing is
spawned.

## `bric_log`

The append-only log, one row per event. `job` is the brief, `key` and
`attempt` identify the attempt, `turn` counts model calls within it.

| `kind`    | what                                                       |
|-----------|------------------------------------------------------------|
| `open`    | attempt claimed; `detail` is the system prompt, tool spec and worker `pid` |
| `reply`   | a model turn; `detail` is its content verbatim, including any thinking |
| `call`    | a tool call the model made; `tool` and `detail` (arguments) |
| `receipt` | a tool result; `text` is its squeezed content, `seq` is what a result row cites as its source |
| `stderr`  | what the worker and its browser wrote to stderr since the last row, if anything |
| `close`   | the row was inserted; `detail` is the submission            |
| `error`   | the attempt failed; `detail` says why                       |

`usage` holds the token counts per turn as JSON. A partial unique index
over `(job, key, attempt)` for `open`, `close` and `error` is the claim:
two workers cannot open the same attempt.

## `bric_page`

FTS5 over `bric_log.text`, filled by trigger as receipts are stored. This is
what the `pages` tool queries; `select * from bric_page where bric_page
match 'x'` works from the shell too.

## `bric_attempt`

One row per `(job, key)`: the last log row's `kind`, `turn`, `ts`, `tool`
and `detail`, with summed `input`, `output` and `cache_read` tokens. This
is the progress and cost view.

## `bric_transcript`

One row per `(job, key, attempt)` with `messages`, the conversation as the
API saw it, rebuilt from the log: the key, each `reply`, and each turn's
receipts as `tool_result` blocks. Together with the `open` row's system
prompt and tools this is the whole request, so any attempt can be replayed
or resumed. Images are not kept.

## Configuration

Everything is an environment variable, read when `run` is called:

| variable            | default                                  | what                                              |
|---------------------|------------------------------------------|---------------------------------------------------|
| `BRIC_KEY`          |                                          | sent as `x-api-key`                               |
| `BRIC_MODEL`        |                                          | model name                                        |
| `BRIC_URL`          | `https://api.anthropic.com/v1/messages`  | any Anthropic-format messages endpoint            |
| `BRIC_TOOLS`        | `browser`                                | MCP servers: a URL, a JSON array of URLs, or a JSON object of URL to allowed tool names; `run`'s fourth argument overrides it. The entry `browser` (or `obscura`) is a browser started for the attempt. A tool name offered by two servers fails the attempt |
| `BRIC_BROWSER`      | `obscura mcp --http`                     | the browser command; `run` appends `--port N`     |
| `BRIC_TURNS`        | `40`                                     | turns per attempt                                 |
| `BRIC_WORKERS`      | `4`                                      | attempts live at once; `run` waits for a slot     |
| `BRIC_SQLITE`       | `sqlite3`                                | the shell workers run in; must be able to `.load` |
| `BRIC_TIMEOUT`      | `120`                                    | seconds per HTTP call                              |
