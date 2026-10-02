# Atelier: a dataset builder with no binary

## Goal

A user describes a dataset. An agent builds it as a
[duckdb.mk](https://github.com/ltrgoddard/duckdb.mk) project: `SELECT`
queries in `models/` that Make builds into Parquet in `build/`. The project
rebuilds with `make` on any machine, with no agent present.

An earlier plan put this in a Go binary with its own agent loop, tools,
validation, logs, sandbox and web interface. Bricolage already has an agent
loop, logs, skills and sandbox wrappers, and duckdb.mk already validates
SQL. This plan builds Atelier from those parts. It adds a skill, a job
definition, a DuckDB macro and one sandbox change. It adds no binary.

## Principles

- DuckDB holds the pipeline. SQLite holds the agent record. A Parquet
  snapshot is the only path between them.
- The project directory and its git history are the state of the dataset.
  Agent edits and hand edits are the same.
- Make is the validator. Atelier does not parse SQL itself.
- Guarantees come from the sandbox and the lock macro. The prompt does not
  provide them.

## Parts

```
bricolage/
  skills/duckdb.mk/SKILL.md      how to work in a project
  skills/duckdb.mk/scripts/      model, query, context, done
  skills/duckdb.mk/lock.sql      the DuckDB lock macro, copied into projects
  example/atelier.sql            the job: request -> edit
  sandbox/seatbelt, bwrap        accept extra writable directories

project/
  Makefile                       includes duckdb.mk
  macros/lock.sql                from the skill
  models/ tests/ data/
  .atelier/<key>/                one git worktree per request (ignored)
  research.db                    optional; see "Research nodes" (ignored)
```

## The job

A request is a row in the source table. Its result is a commit on a branch
where `make` and `make test` pass.

```sql
create table if not exists request (
  key    text primary key,          -- 'r1', 'r2', ...
  parent text references request,   -- the request this one follows
  ask    text not null
);

create table if not exists edit (
  key    text primary key references request,
  branch text not null,
  sha    text not null check (length(sha) = 40),
  note   text not null              -- what changed, one paragraph
);

insert or replace into bric_job (source, target, brief, skills, shell)
values ('request', 'edit',
  'Build what the request asks as a duckdb.mk project. Read SKILL.md.',
  './skills/{duckdb.mk,web}',
  './sandbox/seatbelt /path/to/project');
```

Bricolage sends the source row as JSON, so the agent sees `ask` and
`parent`. A conversation is a chain of requests. For a follow-up, the
agent reads the previous attempt with
`db "select messages from bric_transcript where key = 'r1'"`. Bricolage
needs no change for this: an attempt still answers one key and ends when it
inserts one row.

## Skill scripts

Each script is a few lines of shell. `$P` is the project directory.

- `context` prints a compact description of the project, from one DuckDB
  query: the graph from `build/dag.mmd`, the columns and row count of each
  built table from `parquet_metadata`, the leading comment of each model
  from `read_text('models/**/*.sql')`, and failing tests.
- `query` reads one `SELECT` on stdin and runs it with the lock macro and
  views over the built tables. It returns at most 50 rows. It saves
  nothing.
- `model schema/table` reads one `SELECT` on stdin, writes
  `models/schema/table.sql` in the worktree and runs
  `make build/schema/table.parquet`. It prints the row count, column types
  or the error from Make. duckdb.mk's plan step already refuses anything
  that is not one `SELECT`, so the script does not check this.
- `done` runs `make && make test`, commits the worktree with the request as
  the message, and inserts the `edit` row through `db`. If the build or a
  test fails, it prints the failure and does not insert.

Each request works in its own worktree, `.atelier/<key>`, made from its
parent's commit or from `main`. Parallel requests therefore cannot write to
the same files. `BRIC_WORKERS` limits how many run at once.

## Approval

Approval is a merge. `git diff main..atelier/r2` shows the change. Before a
merge, one check confirms that the agent changed only models and tests:

```sh
git diff --name-only main..atelier/r2 | grep -v '^\(models\|tests\)/' &&
  echo 'refuse: changes outside models/ and tests/'
```

This gives the same result as a tool that accepts only `SELECT`. It is
enforced at the merge, not at each call.

## Sandbox

Two layers apply.

1. The shell wrapper. `sandbox/seatbelt` now lets the agent write only its
   scratch directory and reads nothing under `$HOME`. Change it so that
   extra arguments before the script name more directories to allow, read
   and write. The job passes the project directory. Make the same change
   to `bwrap`. This is the only change to Bricolage's code.
2. The lock macro, `macros/lock.sql`. It applies to every DuckDB run, by
   the agent or by a person, because duckdb.mk reads `macros/` before each
   model:

   ```sql
   load httpfs;
   set allowed_directories = ['.', 'https://example.org/'];
   set enable_external_access = false;
   set lock_configuration = true;
   ```

   Tests on DuckDB 2.0 showed that `allowed_directories` covers URL
   prefixes and refuses `..` paths. `load httpfs` must come first, because
   the extension directory is not on the list. The project edits the host
   list. The agent cannot, because `lock.sql` is outside `models/` and
   `tests/`, and the merge check refuses it.

`make` runs without `BRIC_*` variables, because Bricolage removes them from
the shell. A model cannot read the API key with `getenv`.

## Research nodes

A project can also use Bricolage for research, with a second job in
`research.db`. Treat that file as a source, like a download. Make never
writes it as a side effect and never builds from it directly.

- Keys go in only through `make research`, which nothing depends on. It
  reads keys from an ordinary model, inserts the new ones through DuckDB
  (`attach 'research.db' (type sqlite)`, with an anti-join, because the
  SQLite writer does not support `on conflict`), then loads the extension.
  It refuses if there are more than `BRIC_MAX_NEW` new keys (default 50).
  `make -n research` prints the count.
- Results come out through `sqlite3 research.db '.backup ...'`, which is
  safe while workers write, then a copy to
  `data/research/<table>.parquet`. The file is replaced only if its content
  changed (`cmp -s || mv`), so the log writes do not cause rebuilds. Models
  read this file. Commit it. The project then builds without an API key.
- `tests/research_done.sql` lists source keys with no result. `make test`
  fails while research is pending.
- The snapshot selects named columns with casts, plus `key`, the hash of
  the brief and the model. A test can then find tables that mix briefs.
- Keys come from one macro, for example `lower(trim(x))`. A small change
  upstream then does not start new research for the same entity.

## Observability

Both kinds of agent log to `bric_log` in the Bricolage schema. The menu bar
app shows Atelier requests and research jobs with no change.
`bric_transcript` is the session view, and `bric_attempt` shows progress
and tokens.

A research node's progress is one DuckDB query:

```sql
select job,
  count(distinct key) filter (kind = 'close') as done,
  count(distinct key) filter (kind = 'error') as failed,
  sum((usage->>'output')::int) as output
from sqlite_scan('research.db', 'bric_log')
group by job;
```

For the graph, `build/dag.mmd` renders with Mermaid. To inspect tables,
use `duckdb -ui` or `make shell`. Add a page that combines these only if
the separate tools are not sufficient.

## Concurrency

There are three separate limits. Do not share them.

| Limit | Controls | Resource |
| --- | --- | --- |
| `make -jN` | DuckDB builds | CPU and memory |
| `BRIC_WORKERS` | research agents and Atelier requests | API rate and cost |
| one worktree per request | file writes | the project |

GNU Make's jobserver was considered for a shared limit. It was rejected:
Bricolage workers are detached and outlive Make, so they would keep tokens,
and macOS ships Make 3.81, which has no FIFO jobserver.

## What this design loses

- Approval happens for each request, not for each file. An agent can go
  further in a wrong direction before a person sees the change. Keep
  requests small.
- The agent has `sh`. The sandbox and the merge check contain it, but it
  can try things that a `SELECT`-only tool would not allow. The review
  shows them.
- There is no single page with graph, data and conversation together.
- A conversation is a chain of requests, not one live session. The user
  cannot interrupt an attempt while it runs.

## Milestones

1. **Lock macro.** `skills/duckdb.mk/lock.sql` and tests showing that a
   model cannot read outside the project or reach a host not on the list.
   This is useful without the rest.
2. **Sandbox arguments.** `seatbelt` and `bwrap` accept extra writable
   directories. Tests in `test/`.
3. **Skill and job.** `skills/duckdb.mk/` and `example/atelier.sql`. Done
   when "build a table of X from this CSV URL" gives a branch where `make`
   passes, against `test/fake.py` and against a real model.
4. **Follow-ups.** Requests with `parent`, one worktree each, and the merge
   check.
5. **Research nodes.** The `make research` target, the snapshot rule and
   the done test, as a short Makefile fragment in the skill. Done on a
   project that uses `example/company.sql`.

## Open questions

- Can a pending-count SQL function, `pending(job)`, replace the polling
  queries in `make research` and in the menu bar app?
- Should requests go in through the menu bar app, or is
  `sqlite3 atelier.db "insert into request ..."` enough?
- How large does `context` get on a project with hundreds of models, and
  should it list only the models near the request?
