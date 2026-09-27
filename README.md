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
    make app        # ext/bric-app, a macOS menu bar view of a db
    make app-install  # the same as /Applications/Bricolage.app

Needs a C compiler, libcurl and the SQLite headers. The shell that runs
your script must be able to `.load`: on macOS that is Homebrew's
(`brew install sqlite`; the Makefile finds it at /opt/homebrew/opt/sqlite),
not Apple's. Agents need `sh`, and use `obscura` and `sqlite3` when they
are on the path of the shell you give them.

## Quick start

`company.sql` resolves operators to their parent companies. `lng.sql` is
two jobs in a chain: one lists the export terminals of a coast, and a
trigger on its result table seeds the second, which maps each terminal's
owner, FEED, EPC and equipment suppliers, so one key runs the pipeline.
Loading `company.sql`
creates the `company` to-do table and the `company_parent` result table,
and inserts the job into `bric_job`, so that inserting a key is what
starts an agent:

    sqlite3 research.db < company.sql
    sqlite3 research.db -cmd '.load ./ext/bric' \
      "insert into company values ('Petroleum Development Oman')"

There is no daemon and no queue. A key is pending while the result table
has no row for it, and a commit that inserts keys on a connection with the
extension loaded starts a worker for each pending key up to `BRIC_WORKERS`;
each worker outlives the connection, and each that finishes starts the
next. Keys inserted without the extension start nothing until something
loads it: `sqlite3 research.db ".load ./ext/bric"` restarts whatever is left.
Delete a key's rows from the result table to research it again; editing a
brief reruns nothing.

## Tools

An agent has a shell and web search. It runs scripts in the shell and
queries the database with `db "select ..."`, which also gives it a
full-text search over every page any agent has read. Prompting has three
homes. What the harness needs of every agent, how to submit and what a
receipt is, is in the tool description and never repeated. Who the agent
is and what one row of the job means is the brief, the system prompt, kept
as short as the schema it sits beside. Anything done in the shell, how to
read a page, query some archive or frame a picture, is a skill in the
[Agent Skills](https://agentskills.io) layout, generic enough to share
between jobs; `bric_job.skills` is a glob of the skill directories a job
uses, `./skills/{archive,web}`, and the model gets an index and reads a
skill, and runs its scripts, when it needs to. `skills/web` wraps the
[Obscura](https://github.com/h4ckf0r0day/obscura) browser as `page URL`.

An agent answers by inserting its rows into the result table, and ends the
attempt by ending a turn, so a constraint or trigger your schema carries is
the answer's receipt. There is no submit step and no separate API: SQLite
is the contract. `cites(source, quote)` is the check a cited row needs, that
the quote is on the page the agent read, in one line of a trigger.

Every script and page is a receipt in `bric_log`, numbered and kept with
the script that produced it, so any result can be traced back to, and
rerun from, what the agent read. Receipts are append-only by trigger:
neither an agent nor a slip of yours can edit or delete one. Drop the two
triggers to prune.

## Sandbox

Each `sh` call is one process: `$BRIC_SHELL <script>`, default `sh`, run
in the attempt's scratch directory. That directory is the agent's whole
world: `bin/` holds `db` and every skill script, `skills/` is a copy of each
skill, and `.db` is a unix socket bric serves while the script runs, so
`db "select ..."` reaches the database through curl and nothing else has
to be mounted. The database file, the API key and the worker's `BRIC_*`
environment are never in the sandbox. `sandbox/` holds one executor a
line long for each of the usual isolations; each takes the script path as
its argument and needs nothing outside the scratch directory but the system's own
tools, so the home directory is out of reach:

    BRIC_SHELL=./sandbox/seatbelt      # macOS
    BRIC_SHELL=./sandbox/bwrap         # linux
    BRIC_SHELL=./sandbox/docker        # any; BRIC_IMAGE names the image, default tools

A job can name its own executor in `bric_job.shell`, so a geospatial job
runs in an image with GDAL while the rest use `sh`. Adding a tool is
installing it where that executor can see it, and telling the model about
it in the brief or a skill.

## App

`make app-install` puts Bricolage in /Applications: a menu bar item, a
lowercase b, that watches one database. Each job shows its progress, the
keys its target has out of the keys in its to-do table, and its latest
attempts, from `bric_attempt`. A key typed under a job is inserted into
its to-do table, and a failed one is retried, through Homebrew's `sqlite3`
with the extension loaded and your shell's `BRIC_*` environment, so the app
starts nothing the command line could not. It never writes the database
itself and keeps no state but the path, and quitting it stops no worker.

A job's name opens its result table and a key opens its row, with the
session beside it: the brief, each reply, each script and the receipt it
produced, as `bric_log` has them.

Functions, tables, views and configuration are in [docs/api.md](docs/api.md).
