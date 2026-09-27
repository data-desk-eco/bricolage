# API reference

Loading the extension creates its tables and views and registers four SQL
functions. Jobs require a database file; in-memory databases do not start
background workers.

## Functions

### `run(target, brief, key, shell?)`

Runs one research attempt for `key`. The agent receives the target table's
schema and the instructions in `brief`, then inserts results with `db`.
The target must have a `key` column. If a registered job's source has other
columns, the first message contains that source row as JSON; otherwise it
contains just the key.

The attempt ends when the model finishes a turn without a tool call:

| Return value | Meaning |
| --- | --- |
| `'close'` | The target has a row for this key, including if it existed before the call. |
| `'error'` | The attempt failed; the `bric_log` error entry explains why. |
| `NULL` | Another worker claimed the attempt or all worker slots were occupied. |

Attempts whose worker process has died can be retried. `run` is only
callable directly, not from views or triggers. The optional `shell`
argument overrides `BRIC_SHELL`.

### `alive(pid)`

Returns true if the process exists. Used to detect active workers.

### `cites(source, quote)`

Checks that `quote` occurs on the page at url `source`, in any read of it
kept in `bric_fetch`. Urls match without scheme or trailing slash; the quote is an FTS5 phrase, ignoring case, whitespace and
punctuation. It returns 1 or raises an error that says what to fix: the
page was never read, or the quote is not on it, with the nearest passage.
A validation trigger is one line:

```sql
select cites(new.source, new.quote);
```

Connections inserting into a table with this trigger must load the
extension. The agent's `db` command already has it loaded.

### `squeeze(text)`

Normalizes whitespace, preserving line breaks, and removes base64 data URLs
and invalid UTF-8 bytes. Applied to shell output before storage.

## Jobs: `bric_job`

| Column | Purpose |
| --- | --- |
| `source` | Source table name; primary key. The table must have a `key` column. |
| `target` | Result table name, also used to identify the job in logs. |
| `brief` | Research instructions shared by all keys. |
| `shell` | Shell command; defaults to `BRIC_SHELL`. |
| `model` | Model name; defaults to `BRIC_MODEL`. |
| `params` | Request parameters as JSON; defaults to `BRIC_PARAMS`. |
| `skills` | Pattern matching skill directories, such as `./skills/{archive,web}`. |

Register a job after creating its source and result tables:

```sql
insert or replace into bric_job (source, target, brief)
values ('company', 'company_parent', 'Resolve each operator to its parent company.');
```

Use a separate target for each job. Updating a job affects future workers;
deleting it stops new workers from starting. Neither action stops workers
already running or reruns completed keys.

### Scheduling and retries

The extension checks for pending keys when it loads, after a transaction
inserting source rows commits, and when a worker finishes. Inserts made
without the extension loaded start work on the next load. Rolled-back
transactions start nothing. A trigger that inserts one job's results into
another job's source can chain jobs; see [lng.sql](../example/lng.sql).

A key is pending if it has no result row, no live attempt, and fewer than
`BRIC_ATTEMPTS` failures since its last successful attempt. Workers run in
separate SQLite processes, up to `BRIC_WORKERS` at once, and continue after
the connection that started them closes.

To resume pending work:

```sh
sqlite3 research.db '.load ./ext/bric'
```

Delete a key's result rows to research it again. Increase `BRIC_ATTEMPTS`
to retry keys that have reached the failure limit. To list keys without
results, substitute your table names in:

```sql
select key from source except select key from target;
```

### Request parameters and skills

`params` is applied to the API request using SQLite's `json_patch`. Use it
for endpoint-specific settings such as `max_tokens`. A job's non-null
`params` replaces the `BRIC_PARAMS` setting.

Skills follow the [Agent Skills](https://agentskills.io) directory format.
Paths are relative to the worker's directory. The model receives an index
of skill names, descriptions and paths, and can read each `SKILL.md` as
needed. Skill directories are copied into the attempt's working directory;
files in their `scripts/` directories are linked into `bin/` on `PATH`.

## Logs: `bric_log`

One row per event. `seq` is the event ID, `ts` its timestamp, and `job`,
`key`, `attempt` and `turn` identify the work. `usage` records token counts
as JSON.

| `kind` | Contents |
| --- | --- |
| `open` | System prompt, first message, tools, shell, model, parameters and worker PID in `detail`. |
| `reply` | Model response content in `detail`, including any thinking. |
| `call` | Tool name in `tool`, arguments in `detail`. |
| `receipt` | Tool result. Shell output is stored in `text`. |
| `close` | Successful completion, with result rows as a JSON array in `detail`. |
| `error` | Failure reason in `detail`. |

The log uses `receipt` to mean a tool result. Triggers prevent updates and
deletes. Removing those protections requires dropping `bric_log_update`
and `bric_log_delete`.

## Source text: `bric_fetch` and `bric_page`

`bric_fetch (ts, url, text)` holds every page read, one row per read,
whole, however the agent piped the output; the web skill's `page` writes it.
Join on the url to retrieve a cited source's text:

```sql
select p.*, f.text
from company_parent p
join bric_fetch f on f.url = p.source;
```

`bric_page` is an FTS5 index over that text, shared by all attempts, with
the same `rowid`. Agents and users can search it:

```sql
select rowid, snippet(bric_page, 0, '', '', ' ... ', 48)
from bric_page
where bric_page match 'methane';
```

## Progress: `bric_attempt`

One row per `(job, key)`, showing the latest event and totals across all
attempts for that key.

| Columns | Contents |
| --- | --- |
| `kind`, `turn`, `age` | Latest event, model turn and seconds since the event. |
| `calls`, `images` | Total tool calls and images sent. |
| `read`, `output` | Total input (including cache reads) and output tokens. |
| `job`, `key`, `attempt`, `ts`, `tool`, `detail` | Latest event identifiers and details. |
| `input`, `cache_read` | Separate input and cache-read token totals. |

```sh
sqlite3 -box research.db 'select * from bric_attempt'
```

## Conversations: `bric_transcript`

One row per `(job, key, attempt)`. The `messages` column reconstructs the
conversation from the first message, model replies and tool results.
System prompts and tool definitions are in the corresponding `open` log
entry. Images are not retained, so this is not an exact copy of requests
that included them.

## Shell

Each `sh` call runs the configured shell command, split on whitespace,
with a script path appended. Calls run sequentially in a temporary working
directory shared for the attempt and removed afterward. It contains:

- `bin/db` and links to skill scripts, available on `PATH`.
- `skills/`, containing copies of the selected skills.
- `.script`, the current shell script.
- `.db`, the database socket, served while the script runs.

`db "SQL"` (or SQL on stdin) sends queries through the socket using curl.
It prints CSV with headers, or an error with exit status 22. Queries run on
the worker's connection. Transactions left open at the end of a call are
rolled back.

The shell inherits the worker's environment except for `BRIC_*` variables.
Bricolage adds `BRIC_DB` for the socket path and prepends `bin/` to `PATH`.
Filesystem access depends on the chosen shell wrapper; plain `sh` does not
restrict it. See [sandbox wrappers](../sandbox).

Standard output and errors are combined, with `[exit N]` appended for a
nonzero exit status. PNG and JPEG output is sent to the model as an image.
Other binary output and truncated images are reported by size only.
Text longer than 20,000 characters is shortened in the model's response;
the full stored text remains available in `bric_log`.

After `BRIC_TIMEOUT` seconds or 4 MiB of output, the process group is
killed. The shell wrapper must clean up processes outside that group,
such as detached containers.

## Configuration

Settings are environment variables inherited by workers.

| Variable | Default | Purpose |
| --- | --- | --- |
| `BRIC_KEY` | None | API key, sent as `x-api-key`. |
| `BRIC_MODEL` | None | Model name. |
| `BRIC_URL` | `https://api.anthropic.com/v1/messages` | Anthropic-compatible messages endpoint. |
| `BRIC_PARAMS` | `{}` | JSON parameters applied to the request body. |
| `BRIC_SHELL` | `sh` | Command used to run scripts. |
| `BRIC_ATTEMPTS` | `3` | Failure limit per key since its last success. |
| `BRIC_TURNS` | `40` | Model turns per attempt; the last three prompt the model to finish. |
| `BRIC_WORKERS` | `4` | Maximum concurrent attempts in the database. |
| `BRIC_SQLITE` | `sqlite3` | SQLite CLI used to launch workers; must support `.load`. |
| `BRIC_TIMEOUT` | `120` | Timeout in seconds for HTTP calls, shell calls and waiting for a database write lock. |
