# Bricolage

Bricolage is a SQLite extension for building datasets using LLM research
agents. Inserting rows into a to-do table spawns parallel agents, each
with web search and a shell: the [Obscura](https://github.com/h4ckf0r0day/obscura)
browser, the database itself and whatever else is installed in the sandbox
you name. Each agent writes to a typed result table with validation and
all operations are logged in the database.

Use Bricolage to enrich existing datasets (e.g. take a list of LNG terminal
names, find their operators) or to perform repeated research tasks with a
full audit chain.

## Build

    make            # ext/bric.dylib or ext/bric.so
    make test       # two fake workers against test/fake.py

Needs a C compiler, libcurl and the SQLite headers. The shell that runs
your script must be able to `.load`: on macOS that is Homebrew's
(`brew install sqlite`; the Makefile finds it at /opt/homebrew/opt/sqlite),
not Apple's. Agents need `sh`, and use `obscura` and `sqlite3` when they
are on the path of the shell you give them.

## Quick start

`company.sql` resolves operators to their parent companies. Loading it
creates the `company` to-do table and the `company_parent` result table,
and inserts the job into `bric_job`, so that inserting a key is what
starts an agent:

    sqlite3 research.db < company.sql
    sqlite3 research.db -cmd '.load ./ext/bric' \
      "insert into company values ('Petroleum Development Oman')"

There is no daemon. Each row inserted on a connection with the extension
loaded gets a worker of its own, which outlives the connection; only
`BRIC_WORKERS` run at once and the rest wait for a slot. Keys inserted
without the extension start nothing. To start, or restart, work on every
key that has no result, insert them again:

    insert or replace into company
    select * from company where key not in (select key from company_parent)

## Tools

An agent has a shell, a browser and web search. It runs scripts in the
shell, reads pages with `obscura fetch URL --dump markdown`, and queries
the database with `sqlite3 "$BRIC_DB"`, which also gives it a full-text
search over every page any agent has read. Web search finds pages; the
pages themselves are what get quoted.

An agent answers by inserting its row into the result table, so a
constraint or trigger your schema carries is the answer's receipt. There
is no submit step and no separate API: SQLite is the contract.

Every script and page is a receipt in `bric_log`, numbered and kept with
the script that produced it, so any result can be traced back to, and
rerun from, what the agent read. Receipts are append-only by trigger:
neither an agent nor a slip of yours can edit or delete one. Drop the two
triggers to prune.

## Sandbox

`BRIC_SHELL` is the command each script is piped into, default `sh`. The
extension never learns what isolation is; you compose it from whatever
speaks stdin and stdout:

    BRIC_SHELL='sandbox-exec -f bric.sb sh'                    # macOS seatbelt
    BRIC_SHELL='bwrap --ro-bind / / --tmpfs /tmp --bind . . --bind "$(dirname "$BRIC_DB")" "$(dirname "$BRIC_DB")" --unshare-pid --die-with-parent sh'
    BRIC_SHELL='docker run --rm -i -v "$PWD":"$PWD" -w "$PWD" -v "$(dirname "$BRIC_DB")":"$(dirname "$BRIC_DB")" -e BRIC_DB tools sh'

The command runs with the attempt's scratch directory as its working
directory and `BRIC_DB` set to the database's path, so `$PWD` and
`$BRIC_DB` in a Docker line mount both. The database's directory must be
writable from inside, since the answer is an insert and WAL keeps its
journal beside the file. A job can name its own sandbox in
`bric_job.shell`, so a geospatial job runs in an image with GDAL while the
rest use `sh`. Adding a tool is installing it where that shell can see it,
and telling the model about it in the brief.

Functions, tables, views and configuration are in [docs/api.md](docs/api.md).
