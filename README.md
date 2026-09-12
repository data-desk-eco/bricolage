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

The model gets three kinds of tool:

- **Web search** from the provider. The request includes Anthropic's
  `web_search` server tool, which works on Anthropic's API and on DeepSeek's
  Anthropic-format endpoint. Search results are for finding pages, not for
  citing.
- **A browser**, [Obscura](https://github.com/h4ckf0r0day/obscura), started
  for each attempt on a free port and killed when the attempt ends. Every
  page it returns is stored, squeezed, in `bric_log.text`. A result row
  cites the receipt it quotes by `seq`, so a trigger like
  `company_parent_cite` can reject a quote that is not on the page. Set
  `BRIC_BROWSER` to run it differently.
- **Anything over MCP.** Set `BRIC_TOOLS` to a server URL, a list of them,
  or a map of URL to allowed tool names. These tools replace the browser
  and their receipts are logged the same way.

Functions, tables, views and configuration are in [docs/api.md](docs/api.md).

