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

The model has a shell and web search, and answers with a SQL insert.

- **`sh`** runs a script and returns what it printed. Every receipt is
  stored squeezed in `bric_log.text` with a `seq`, and a result row cites
  the receipt it quotes by that number, so a trigger like
  `company_parent_cite` can reject a quote that is not in it. The script
  is in the `call` row, so any receipt in the database can be rerun. The
  working directory is a scratch directory kept for the attempt and deleted
  after it; stdout that is a PNG or JPEG is shown to the model as an image.
  From the shell, `obscura fetch URL --dump markdown` is the browser and
  `sqlite3 "$BRIC_DB"` is this database: `bric_page` is a full-text
  search over every page any attempt has read, so the hundredth operator
  is resolved against the pages the first ninety-nine read, and finished
  result tables are there to query.
- **The answer is an insert.** The model writes its row into the result
  table with `sqlite3`, against the DDL in its system prompt. A constraint
  or trigger failure is its receipt. Once a row for the key exists at the
  end of a turn, the attempt is closed with that row as the record. There
  is no submit tool: SQLite is the contract and the transport.
- **Web search** from the provider, via Anthropic's `web_search` server
  tool. Works on Anthropic's API and DeepSeek's Anthropic-format endpoint.
  For finding pages, not citing them.

Because the model writes to the database, `bric_log` is append-only by
trigger: neither an agent nor a slip of yours can edit or delete a receipt.
Drop the two triggers to prune.

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
