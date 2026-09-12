# Bricolage

Bricolage is a SQLite extension for building datasets using LLM research agents. Agents `SELECT` work from a shared to-do list defined as a table, browse the web and call tools over MCP, then write to a result table with typed columns, source URLs and optional validation triggers, with everything logged. Use Bricolage to enrich existing datasets (e.g. take a list of LNG terminal names, find their operators) or to perform repeated research tasks with a full audit chain.

## Build

    make            # ext/bric.dylib or ext/bric.so
    make test       # two fake workers against test/fake.py

Needs a C compiler, libcurl and the SQLite headers. The shell that runs
your script must be able to `.load`: on macOS that is Homebrew's
(`brew install sqlite`; the Makefile finds it at /opt/homebrew/opt/sqlite),
not Apple's.

## Quick start

`company.sql` resolves operators to their parent companies:

    sqlite3 research.db "create table company (key text primary key);
                         insert into company values ('Petroleum Development Oman');"
    export BRIC_URL=https://api.deepseek.com/anthropic/v1/messages   # or Anthropic's, the default
    export BRIC_MODEL=deepseek-flash BRIC_KEY=...
    make run DB=research.db SCRIPT=company.sql WORKERS=4
    sqlite3 research.db 'select * from company_parent; select * from bric_attempt'

`make run` starts one sqlite3 per worker and prints the tally when they
finish; `SQLITE` overrides what it finds. The line it runs per worker is

    sqlite3 research.db < company.sql

so any process manager does as well. Each attempt gets a fresh Obscura,
started by `run` on a free port and killed when the attempt ends, so a
worker's memory is one key's pages, not the heaviest page it ever saw.
Set `BRIC_TOOLS` to bring your own server instead. A build of Obscura with
`--features render` adds `browser_screenshot`, and the model can then read
pictures.

## API

Loading the extension adds two functions, one table and one view.

### `run(target, brief, key, tools?)`

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

### `squeeze(text)`

Collapses runs of whitespace to one space and strips base64 data URLs. Every
receipt is stored squeezed, so apply it to a quote before matching it
against `bric_log.text`, as the trigger in `company.sql` does.

### `bric_log`

The append-only log, one row per event. `job` is the brief, `key` and
`attempt` identify the attempt, `turn` counts model calls within it.

| `kind`    | what                                                       |
|-----------|------------------------------------------------------------|
| `open`    | attempt claimed; `detail` is the tool spec                 |
| `call`    | a tool call the model made; `tool` and `detail` (arguments) |
| `receipt` | a tool result; `text` is its squeezed content, `seq` is what a result row cites as its source |
| `close`   | the row was inserted; `detail` is the submission            |
| `error`   | the attempt failed; `detail` says why                       |

`usage` holds the token counts per turn as JSON. A partial unique index
over `(job, key, attempt)` for `open`, `close` and `error` is the claim:
two workers cannot open the same attempt.

### `bric_attempt`

One row per `(job, key)`: the last log row's `kind`, `turn`, `ts`, `tool`
and `detail`, with summed `input`, `output` and `cache_read` tokens. This
is the progress and cost view.

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
| `BRIC_TIMEOUT`      | `120`                                    | seconds per HTTP call; an attempt silent for twice this is dead |
