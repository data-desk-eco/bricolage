---
name: web
description: finding and reading pages on the web; `page URL` reads one through a browser as markdown, links kept
---
# the web

The web search tool finds pages; a page is read with

    page https://example.org/permit/1234            # markdown, links kept
    page https://example.org/permit/1234 text       # its text alone
    page https://example.org/permit/1234 links      # the links alone
    page https://example.org/permit/1234 shot       # a png of it, shown to you
    page https://example.org/permit/1234.pdf        # a pdf's text

Every page read is kept whole in `bric_fetch`, so pipe `page` through
`grep` or `head` freely and cite the url itself: the quote is checked
against the whole page, whatever you printed of it. To see more of a page
you have read, query it rather than fetching it again:

    db "select substr(text, 1, 20000) from bric_fetch where url = 'https://…'"

Prefer a regulator's record, a permit, an operator's own page or a dated
report that names the thing over a search result or a news roundup, and
search in the local language when that is where the thing is. A search
result is a lead, not evidence: read it before you cite it.

A page that fails or comes back as a cookie wall or a login prompt will do
the same next time; try another copy of it (the operator's own release, an
archive.org snapshot) rather than the same url again. A MediaWiki site
that blocks the browser often answers its api:
`page 'https://site/w/api.php?action=parse&page=Title&format=json&prop=wikitext'`.

Three searches with nothing useful means stop. Many things have no page,
and what you already hold is then the evidence.
