# API

Loading the extension adds four functions, two tables and two views.

## `job(source, target, brief, tools?)`

Registers a job in `bric_job`: keys are rows of `source` (a table with a
`key` column) that have no row in `target`. From then on, on any
connection with the extension loaded, an insert into `source` forks a
worker unless `BRIC_WORKERS` are already live. `job` itself forks one too,
to pick up whatever is pending. Workers outlive the connection.

## `drain(target, brief, source, tools?)`

What the worker runs: `run` over each pending key, in the foreground,
repeating until a pass makes no progress. A key that has failed `BRIC_TRIES`
times is left alone.

## `run(target, brief, key, tools?)`

Runs one research attempt for `key` and inserts the answer into `target`.
`target` names a table you have created; its DDL, with any `CHECK`
constraints and triggers, is the contract the model writes against and the
one SQLite enforces. `brief` is the task in prose, shared by every key in
the job. `tools` is an optional MCP spec in the same form as `BRIC_TOOLS`.

Returns `'close'` when the row was inserted, `'error'` when the model gave
up, and `NULL` when the attempt did not finish: the key was claimed by
another worker, the tool server could not be reached, or the turn cap was
hit. A key that already has a close row returns `'close'` without a model
call, and an attempt silent for twice `BRIC_TIMEOUT` is treated as dead and
retried, so a `SELECT run(...)` over the whole to-do list is safe to rerun. Only callable at the top level
of a statement, not from views or triggers.

## `squeeze(text)`

Collapses runs of whitespace to one space and strips base64 data URLs. Every
receipt is stored squeezed, so apply it to a quote before matching it
against `bric_log.text`, as the trigger in `company.sql` does.

## `bric_log`

The append-only log, one row per event. `job` is the brief, `key` and
`attempt` identify the attempt, `turn` counts model calls within it.

| `kind`    | what                                                       |
|-----------|------------------------------------------------------------|
| `open`    | attempt claimed; `detail` is the system prompt and tool spec |
| `reply`   | a model turn; `detail` is its content verbatim, including any thinking |
| `call`    | a tool call the model made; `tool` and `detail` (arguments) |
| `receipt` | a tool result; `text` is its squeezed content, `seq` is what a result row cites as its source |
| `close`   | the row was inserted; `detail` is the submission            |
| `error`   | the attempt failed; `detail` says why                       |

`usage` holds the token counts per turn as JSON. A partial unique index
over `(job, key, attempt)` for `open`, `close` and `error` is the claim:
two workers cannot open the same attempt.

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
| `BRIC_TOOLS`        |                                          | MCP servers: a URL, a JSON array of URLs, or a JSON object of URL to allowed tool names; `run`'s fourth argument overrides it. Unset, `run` starts a browser per attempt |
| `BRIC_BROWSER`      | `obscura mcp --http`                     | the browser command; `run` appends `--port N`     |
| `BRIC_TURNS`        | `40`                                     | turns per attempt                                 |
| `BRIC_WORKERS`      | `4`                                      | workers an insert will have live at once          |
| `BRIC_TRIES`        | `3`                                      | errors before `drain` gives up on a key           |
| `BRIC_SQLITE`       | `sqlite3`                                | the shell workers run in; must be able to `.load` |
| `BRIC_TIMEOUT`      | `120`                                    | seconds per HTTP call; an attempt silent for twice this is dead |
