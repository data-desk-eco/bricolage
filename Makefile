S := $(if $(filter Darwin,$(shell uname -s)),dylib,so)
CFLAGS ?= -O2 -Wall -Wextra -Wno-missing-field-initializers $(if $(wildcard /opt/homebrew/opt/sqlite/include),-I/opt/homebrew/opt/sqlite/include)
SQLITE ?= $(if $(wildcard /opt/homebrew/opt/sqlite/bin/sqlite3),/opt/homebrew/opt/sqlite/bin/sqlite3,sqlite3)
DB ?= research.db
SCRIPT ?= company.sql
WORKERS ?= 4

ext/sql.h: sql/*.sql
	@mkdir -p ext
	@for f in $^; do printf 'static const char sql_%s[] =\n' $$(basename $$f .sql); sed 's/\\/\\\\/g; s/"/\\"/g; s/^/"/; s/$$/\\n"/' $$f; echo ';'; done > $@

ext/bric.$(S): src/bric.c ext/sql.h
	$(CC) $(CFLAGS) -Iext -fPIC -shared $< -lcurl -o $@

run: ext/bric.$(S)
	@trap 'kill $$(jobs -p) 2>/dev/null; wait' EXIT INT TERM; \
	for i in $$(seq $(WORKERS)); do $(SQLITE) $(DB) < $(SCRIPT) >/dev/null & done; wait; \
	$(SQLITE) -header -column $(DB) "select kind, count(*) as keys, sum(input) as input, sum(output) as output from bric_attempt group by kind"

test: ext/bric.$(S)
	SQLITE=$(SQLITE) python3 test/fake.py

clean:
	rm -rf ext test/out.db*

.PHONY: run test clean
