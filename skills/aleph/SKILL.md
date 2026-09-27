---
name: aleph
description: searching and reading a leak held on an aleph server; `aleph search Q`, `aleph read URL`, and any api path raw
---
# aleph

An [Aleph](https://docs.aleph.occrp.org) server holds leaked and public
collections as entities: documents, emails, tables, pages, people and
companies. The script talks to its api, as set up for you:

    aleph search 'fuel oil "bill of lading"' filter:collection_id=12119
    aleph read https://aleph.example.org/entities/80c1…ffe0
    aleph /collections q=sonatrach                # any GET path, as json
    aleph /entities filter:properties.parent=ID   # a folder's contents

`search` prints the hit count, then one tab-separated line a hit: the
entity's url, schema, collection, title, date and the matching snippets.
Queries take Elasticsearch syntax: `"exact phrase"`, `OR`, `-word`,
`name~` for fuzzy spelling, and filters such as `filter:schemata=Email`,
`filter:dates=2018-01..2018-12` and `offset=20` for the next page. A leak
is often in Russian, Spanish or French as well as English: search both.

`read` prints an entity's properties and its full text, every page of a
pdf, and keeps that text in `bric_fetch` under the entity's url. Cite that
url, with a quote from what `read` printed; a search snippet is a lead, not
a source, until you read the entity. Pipe `read` through `grep` or `head`
freely, and query `bric_fetch` rather than reading the same entity again.

A document's `parent` is its folder or the email or archive it came in,
and its siblings are often the rest of the same deal: an invoice beside a
bill of lading beside a contract. OCR of a scan is rough, so quote a clean
run of words from it, not a garbled table.
