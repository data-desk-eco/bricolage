# bric

research is a table you declare. the model fills it, sqlite validates it.
[SPEC.md](SPEC.md) is the whole description.

## build

    make            # ext/bric.dylib or ext/bric.so
    make test       # two fake workers against test/fake.py

needs a c compiler, libcurl and the sqlite3 headers. the shell that runs
your script must be able to `.load`: on macos that is homebrew's
(`brew install sqlite`; the makefile finds it at /opt/homebrew/opt/sqlite),
not apple's.

## quick start

`company.sql` resolves operators to their parent companies:

    sqlite3 research.db "create table company (key text primary key);
                         insert into company values ('Petroleum Development Oman');"
    export BRIC_URL=https://api.deepseek.com/anthropic/v1/messages   # or anthropic's, the default
    export BRIC_MODEL=deepseek-flash BRIC_KEY=...
    make run DB=research.db SCRIPT=company.sql WORKERS=4
    sqlite3 research.db 'select * from company_parent; select * from bric_attempt'

`make run` starts one sqlite3 per worker and prints the tally when they
finish; `SQLITE` overrides what it finds. the line it runs per worker is

    sqlite3 research.db < company.sql

so any process manager does as well. each attempt gets a fresh obscura,
started by `run` on a free port and killed when the attempt ends, so a
worker's memory is one key's pages, not the heaviest page it ever saw.
set `BRIC_TOOLS` to bring your own server instead. a build of obscura with
`--features render` adds `browser_screenshot`, and the model can then read
pictures.

## configuration

everything is an environment variable, read when `run` is called:

| variable            | default                                  | what                                              |
|---------------------|------------------------------------------|---------------------------------------------------|
| `BRIC_KEY` |                                          | sent as `x-api-key`                               |
| `BRIC_MODEL`        |                                          | model name                                        |
| `BRIC_URL`          | `https://api.anthropic.com/v1/messages`  | any anthropic-format messages endpoint            |
| `BRIC_TOOLS`        |                                          | mcp servers: a url, a json array of urls, or a json object of url to allowed tool names; `run`'s fourth argument overrides it. unset, `run` starts a browser per attempt |
| `BRIC_BROWSER`      | `obscura mcp --http`                     | the browser command; `run` appends `--port N` |
| `BRIC_TURNS`        | `40`                                     | turns per attempt                                 |
| `BRIC_TIMEOUT`      | `120`                                    | seconds per http call; an attempt silent for twice this is dead |

the sql surface is `run(target, brief, key, tools?)`, `squeeze(text)` and
the `bric_attempt` view; `bric_log` is the table under it. see the spec.
