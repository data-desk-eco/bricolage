# Bricolage

Bricolage is a SQLite extension for building datasets using LLM research
agents. Agents `SELECT` work from a shared to-do list defined as a table,
browse the web and call tools over MCP, then write to a result table with
typed columns, source URLs and optional validation triggers, with everything
logged.

Use Bricolage to enrich existing datasets (e.g. take a list of LNG terminal
names, find their operators) or to perform repeated research tasks with a
full audit chain.

## Build

    make            # ext/bric.dylib or ext/bric.so
    make test       # two fake workers against test/fake.py

Needs a C compiler, libcurl and the SQLite headers. The shell that runs
your script must be able to `.load`: on macOS that is Homebrew's
(`brew install sqlite`; the Makefile finds it at /opt/homebrew/opt/sqlite),
not Apple's.

## Quick start

`company.sql` resolves operators to their parent companies. Loading it
creates the `company` to-do table and the `company_parent` result table,
and registers the job, so that inserting a key is what starts an agent:

    export BRIC_URL=https://api.deepseek.com/anthropic/v1/messages   # or Anthropic's, the default
    export BRIC_MODEL=deepseek-flash BRIC_KEY=... BRIC_SQLITE=/opt/homebrew/opt/sqlite/bin/sqlite3
    sqlite3 research.db < company.sql
    sqlite3 research.db -cmd '.load ./ext/bric' \
      "insert into company values ('Petroleum Development Oman')"
    sqlite3 research.db 'select * from company_parent; select * from bric_attempt'

There is no daemon. Each row inserted on a connection with the extension
loaded gets a worker of its own, which outlives the connection; only
`BRIC_WORKERS` run at once and the rest wait for a slot. Keys inserted
without the extension start nothing. To start, or restart, work on every
key that has no result, insert them again:

    insert or replace into company
    select * from company where key not in (select key from company_parent)

Each attempt gets its own Obscura browser, or set `BRIC_TOOLS` to bring
your own MCP server.

Functions, tables, views and configuration are in [docs/api.md](docs/api.md).

