S := $(if $(filter Darwin,$(shell uname -s)),dylib,so)
CFLAGS ?= -O2 -Wall -Wextra -Wno-missing-field-initializers $(if $(wildcard /opt/homebrew/opt/sqlite/include),-I/opt/homebrew/opt/sqlite/include)
SQLITE ?= $(if $(wildcard /opt/homebrew/opt/sqlite/bin/sqlite3),/opt/homebrew/opt/sqlite/bin/sqlite3,sqlite3)

ext/bric.$(S): src/bric.c src/sql.h
	@mkdir -p ext
	$(CC) $(CFLAGS) -fPIC -shared $< -lcurl -o $@

test: ext/bric.$(S)
	SQLITE=$(SQLITE) python3 test/fake.py

clean:
	rm -rf ext test/out.db* test/out.bin

.PHONY: test clean

app: ext/bric-app
ext/bric-app: app/bric.swift
	@mkdir -p ext
	swiftc -O -parse-as-library $< -o $@
.PHONY: app

# a menu bar bundle (LSUIElement: no dock icon) copied to /Applications
A = ext/Bricolage.app/Contents
app-install: ext/bric-app
	mkdir -p $(A)/MacOS && mkdir -p $(A)/Resources && cp app/bric.icns $(A)/Resources
	cp $< $(A)/MacOS/bric
	plutil -create xml1 $(A)/Info.plist
	for k in CFBundleExecutable=bric CFBundleIdentifier=eco.datadesk.bric \
	  CFBundleName=Bricolage CFBundleIconFile=bric CFBundlePackageType=APPL; do \
	  plutil -insert $${k%%=*} -string $${k#*=} $(A)/Info.plist; done
	plutil -insert LSUIElement -bool true $(A)/Info.plist
	codesign -fs - ext/Bricolage.app
	rm -rf /Applications/Bricolage.app && cp -R ext/Bricolage.app /Applications
.PHONY: app-install
