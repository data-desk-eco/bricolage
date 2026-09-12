S := $(if $(filter Darwin,$(shell uname -s)),dylib,so)
CFLAGS ?= -O2 -Wall -Wextra -Wno-missing-field-initializers $(if $(wildcard /opt/homebrew/opt/sqlite/include),-I/opt/homebrew/opt/sqlite/include)
SQLITE ?= $(if $(wildcard /opt/homebrew/opt/sqlite/bin/sqlite3),/opt/homebrew/opt/sqlite/bin/sqlite3,sqlite3)
OBSCURA ?= obscura
DB ?= research.db
SCRIPT ?= company.sql
WORKERS ?= 4
PORT ?= 3000

ext/bric.$(S): src/bric.c
	@mkdir -p ext
	$(CC) $(CFLAGS) -fPIC -shared $< -lcurl -o $@

run: ext/bric.$(S)
	@trap 'kill $$(jobs -p) 2>/dev/null; wait' EXIT INT TERM; \
	for i in $$(seq 0 $$(($(WORKERS) - 1))); do $(OBSCURA) mcp --http --port $$(($(PORT) + i)) >/dev/null 2>&1 & done; \
	for i in $$(seq 0 $$(($(WORKERS) - 1))); do \
	  until curl -so /dev/null http://127.0.0.1:$$(($(PORT) + i))/mcp; do sleep 0.2; done; \
	  BRIC_TOOLS=http://127.0.0.1:$$(($(PORT) + i))/mcp $(SQLITE) $(DB) < $(SCRIPT) >/dev/null & \
	done; \
	wait $$(jobs -p | tail -n $(WORKERS)); \
	$(SQLITE) -header -column $(DB) "select kind, count(*) as keys, sum(input) as input, sum(output) as output from bric_attempt group by kind"

test: ext/bric.$(S)
	SQLITE=$(SQLITE) python3 test/fake.py

clean:
	rm -rf ext test/out.db*

.PHONY: run test clean
