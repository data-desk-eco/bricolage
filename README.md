# bric

research is a table you declare. the model fills it, sqlite validates it.
[SPEC.md](SPEC.md) is the whole description; `company.sql` the example.

    make                      # ext/bric.dylib or .so; needs sqlite3 headers and libcurl
    make ext/sqlite3          # a shell that can .load, if yours cannot
    make test                 # against test/fake.py
    export BRIC_MODEL=... ANTHROPIC_API_KEY=... BRIC_URL=...   # url optional
    BRIC_TOOLS='["http://127.0.0.1:3001/mcp"]' sqlite3 research.db < company.sql
