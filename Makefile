S := $(if $(filter Darwin,$(shell uname -s)),dylib,so)
CFLAGS ?= -O2 -Wall -Wextra -Wno-missing-field-initializers $(if $(wildcard /opt/homebrew/opt/sqlite/include),-I/opt/homebrew/opt/sqlite/include)
SQLITE ?= $(if $(wildcard /opt/homebrew/opt/sqlite/bin/sqlite3),/opt/homebrew/opt/sqlite/bin/sqlite3,sqlite3)
DB ?= research.db
SCRIPT ?= company.sql

ext/bric.$(S): src/bric.c src/sql.h
	@mkdir -p ext
	$(CC) $(CFLAGS) -fPIC -shared $< -lcurl -o $@

run: ext/bric.$(S)
	BRIC_SQLITE=$(SQLITE) $(SQLITE) $(DB) < $(SCRIPT)

test: ext/bric.$(S)
	SQLITE=$(SQLITE) python3 test/fake.py

clean:
	rm -rf ext test/out.db*

.PHONY: run test clean
