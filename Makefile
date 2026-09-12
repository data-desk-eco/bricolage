S := $(if $(filter Darwin,$(shell uname -s)),dylib,so)
CFLAGS ?= -O2 -Wall -Wextra -Wno-missing-field-initializers $(if $(wildcard /opt/homebrew/opt/sqlite/include),-I/opt/homebrew/opt/sqlite/include)
SQLITE ?= $(if $(wildcard ext/sqlite3),ext/sqlite3,sqlite3)

ext/bric.$(S): src/bric.c
	@mkdir -p ext
	$(CC) $(CFLAGS) -fPIC -shared $< -lcurl -o $@

SQLITE_VER ?= 3510000
ext/sqlite3:
	@mkdir -p ext/sqlite-build && cd ext/sqlite-build && curl -fsSLO https://www.sqlite.org/2025/sqlite-amalgamation-$(SQLITE_VER).zip && unzip -qo sqlite-amalgamation-$(SQLITE_VER).zip
	$(CC) -O2 -DSQLITE_ENABLE_LOAD_EXTENSION -DSQLITE_ENABLE_MATH_FUNCTIONS ext/sqlite-build/sqlite-amalgamation-$(SQLITE_VER)/{shell,sqlite3}.c -o $@ -lm -lpthread

test: ext/bric.$(S)
	SQLITE=$(SQLITE) python3 test/fake.py

clean:
	rm -f ext/bric.$(S)

.PHONY: test clean
