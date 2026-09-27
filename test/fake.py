import json, os, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

URL = 'https://example.org/acme'
PAGE = URL + '\nAcme Ltd   is a wholly owned\n\n\tsubsidiary of   Globex Corporation. ' + 'filler text. ' * 3000
QUOTE = 'wholly owned\nsubsidiary of Globex Corporation'
PNG = bytes.fromhex('89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d4944415478da63f8cfc0f01f0005000201cae1a5d90000000049454e44ae426082')
PNG64 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP4z8DwHwAFAAIByuGl2QAAAABJRU5ErkJggg=='
SQLITE = os.environ.get('SQLITE', 'sqlite3')


def keyof(message):
    return json.loads(message)['key'] if message.startswith('{') else message


def sh(id, script):
    return {'type': 'tool_use', 'id': id, 'name': 'sh', 'input': {'command': script}}


def insert(id, table, **row):
    vals = ', '.join("'%s'" % v.replace("'", "''") if isinstance(v, str) else str(v) for v in row.values())
    return sh(id, 'db <<\'EOF\'\ninsert into %s (%s) values (%s);\nEOF' % (table, ', '.join(row), vals))


class H(BaseHTTPRequestHandler):
    calls = []

    def log_message(self, *a):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['content-length'])))
        H.calls.append(req)
        key = keyof(req['messages'][0]['content'])
        paused = key == 'bolt' and req['messages'][-1]['role'] == 'assistant'
        turn = sum(m['role'] == 'assistant' for m in req['messages'])
        turn -= key == 'bolt' and turn > 0
        if paused:
            assert req['messages'][-1]['content'][0]['type'] == 'server_tool_use', req['messages'][-1]
        blocks = [c['content'] for m in req['messages'] if m['role'] == 'user' and isinstance(m['content'], list) for c in m['content']]
        results = [b[0]['text'] if isinstance(b, list) else b for b in blocks]
        last = results[-1] if results else ''
        md = any(QUOTE in r for r in results)
        quote = 'wholly   owned\nsubsidiary of Globex Corporation'
        if req['system'].startswith('bare shell'):
            if turn == 0:
                content = [sh('b1', 'echo "X=$X"')]
            elif turn == 1:
                assert last == 'X=1', last
                content = [sh('b2', 'db "insert into bare values (\'%s\', cast(\'Globex\' as blob))"' % key)]
            elif turn == 2:
                content = [sh('b3', 'db "begin; insert into bare values (\'x\', \'y\'); insert into bare values (\'x\', \'z\'); commit;"')]
            else:
                assert 'UNIQUE' in last and 'rolled back' in last, last
                content = [{'type': 'text', 'text': 'done'}]
        elif key == 'plain':
            content = [{'type': 'text', 'text': 'no idea'}]
        elif key == 'bolt' and turn == 0 and not paused:
            content = [{'type': 'server_tool_use', 'id': 's1', 'name': 'web_search', 'input': {'query': 'bolt'}}]
        elif turn == 0:
            content = [sh('c1', 'page https://example.org/acme'),
                       sh('c2', "printf '%s'" % ''.join('\\%03o' % b for b in PNG)),
                       sh('c3', 'echo "K=$BRIC_KEY|D=$BRIC_DB|P=$PWD"; printf \'\\377\\303\'; db "select count(*) from bric_log where key = \'%s\' and kind = \'receipt\' and attempt = (select max(attempt) from bric_log where key = \'%s\')"; echo x > f; exit 3' % (key, key))]
        elif turn == 1:
            assert md, results
            images = [x['source'] for b in blocks if isinstance(b, list) for x in b[1:]]
            assert images == [{'type': 'base64', 'media_type': 'image/png', 'data': PNG64}], images
            assert results[-2] == '[image/png, %d bytes]' % len(PNG), results[-2]
            assert last.startswith('K=|D='), last
            assert last.endswith('\n2\n[exit 3]'), last
            content = [sh('c4', 'cat f; sleep 5'), insert('c5', 'results', key=key, parent='Globex', confidence='certain', source=URL, quote=quote)]
        elif turn == 2:
            assert results[-2] == 'x\n[killed after timeout]', results[-2]
            assert 'confidence' in last, last
            content = [insert('c6', 'results', key=key, parent='Globex', confidence='high', source=URL, quote='owned outright by Initech')]
        elif turn == 3:
            assert 'quote not found' in last, last
            content = [insert('c7', 'results', key=key, parent='Globex', confidence='high', source=URL, quote=quote)]
        else:
            content = [{'type': 'text', 'text': 'giving up'}]
        time.sleep(0.2)
        # deepseek stops a turn of only server searches with tool_use
        stop = 'tool_use' if content[0]['type'].endswith('tool_use') else 'end_turn'
        body = json.dumps({'content': content, 'stop_reason': stop, 'usage': {'input_tokens': 10, 'output_tokens': 5, 'cache_read_input_tokens': 1}}).encode()
        self.send_response(200)
        self.send_header('content-type', 'application/json')
        self.send_header('content-length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


SQL = '''
.load ./ext/bric
create table if not exists results (
  key text primary key,
  parent text not null,
  confidence text not null check (confidence in ('high', 'medium', 'low')),
  source text not null,
  quote text not null
);
create trigger if not exists results_cite before insert on results
begin
  select cites(new.source, new.quote);
end;
select run('results', 'Resolve each operator to its parent.', key) from company where key not in (select key from results);
'''


def sqlite(*sql, db='test/out.db'):
    return subprocess.run([SQLITE, db], input='\n'.join(sql), capture_output=True, text=True)


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), H)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:%d' % server.server_port
    os.makedirs('test/out.bin', exist_ok=True)
    with open('test/out.bin/page', 'w') as f:
        f.write("#!/bin/sh\ncat <<'EOF'\n%s\nEOF\ndb \"insert into bric_fetch (url, text) values ('%s', squeeze('%s'))\" >/dev/null\n" % (PAGE, URL, PAGE.split('\n', 1)[1]))
    os.chmod('test/out.bin/page', 0o755)
    os.environ['PATH'] = os.path.abspath('test/out.bin') + ':' + os.environ['PATH']
    os.environ.update(SQLITE=SQLITE, BRIC_SQLITE=SQLITE, BRIC_URL=base + '/v1/messages', BRIC_MODEL='fake', BRIC_KEY='secret', BRIC_TIMEOUT='2')
    for f in ['test/out.db', 'test/out.db-wal', 'test/out.db-shm']:
        if os.path.exists(f):
            os.remove(f)
    r = sqlite('create table company (key text primary key, parent text, source integer);',
               "insert into company (key) values ('acme'), ('bolt'), ('cog'), ('plain');")
    assert not r.returncode, r.stderr
    workers = [subprocess.Popen([SQLITE, 'test/out.db'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(2)]
    outs = [w.communicate(SQL) for w in workers]
    for w, (out, err) in zip(workers, outs):
        assert not w.returncode, err
    r = sqlite('select key, parent, source, quote from results order by key;')
    assert r.stdout == ''.join('%s|Globex|%s|wholly   owned\nsubsidiary of Globex Corporation\n' % (k, URL) for k in ['acme', 'bolt', 'cog']), r.stdout
    r = sqlite("select key, sum(kind = 'open'), sum(kind = 'close'), sum(kind = 'error') from bric_log where kind in ('open', 'close', 'error') group by 1 order by 1;")
    assert r.stdout in ('acme|1|1|0\nbolt|1|1|0\ncog|1|1|0\nplain|%d|0|%d\n' % (n, n) for n in (1, 2)), r.stdout
    r = sqlite("select count(*) from bric_log where kind = 'receipt' and text is null;")
    assert r.stdout == '0\n', r.stdout
    r = sqlite("select distinct detail from bric_log where key = 'plain' and kind = 'error';")
    assert r.stdout == 'reply without submission: no idea\n', r.stdout
    r = sqlite("select count(*) = (select count(*) from bric_fetch), count(*) > 2 from bric_page where bric_page match 'globex';")
    assert r.stdout == '1|1\n', r.stdout
    r = sqlite("select kind, input, output, calls, images, age < 60 from bric_attempt where key = 'acme';")
    assert r.stdout == 'close|50|25|7|1|1\n', r.stdout
    r = sqlite("select json_array_length(detail -> 'tools'), detail ->> 'shell', detail -> 'tools' ->> '$[1].name' from bric_log where key = 'acme' and kind = 'open';")
    assert r.stdout == '2|sh|sh\n', r.stdout
    r = sqlite("select instr(text, '  '), detail like '%chars: https://example.org/acme' from bric_log where key = 'acme' and kind = 'receipt' and tool = 'sh' order by seq limit 1;")
    assert r.stdout == '0|1\n', r.stdout
    r = sqlite("select group_concat(distinct url), min(instr(text, '  ')) from bric_fetch;")
    assert r.stdout == URL + '|0\n', r.stdout
    r = sqlite("select detail like '%; 1 image % chars', text from bric_log where key = 'acme' and kind = 'receipt' and tool = 'sh' order by seq limit 1 offset 1;")
    assert r.stdout == '1|[image/png, %d bytes]\n' % len(PNG), r.stdout
    r = sqlite("select detail ->> 'command' from bric_log where key = 'acme' and kind = 'call' and tool = 'sh' order by seq limit 1 offset 3;")
    assert r.stdout == 'cat f; sleep 5\n', r.stdout
    r = sqlite("select detail ->> '$[0].parent', detail ->> '$[0].key' from bric_log where key = 'acme' and kind = 'close';")
    assert r.stdout == 'Globex|acme\n', r.stdout
    r = sqlite("update bric_log set text = 'forged' where key = 'acme' and kind = 'receipt';")
    assert 'append-only' in r.stderr, r.stderr
    r = sqlite("drop trigger bric_log_update; drop trigger bric_log_delete;",
               "delete from bric_log where key = 'plain';",
               "insert into bric_log (ts, job, key, attempt, turn, kind) values (datetime('now', '-1 hour'), 'results', 'plain', 1, 3, 'call');",
               "delete from results where key = 'acme';")
    r = sqlite(SQL)
    assert not r.returncode, r.stderr
    r = sqlite("select key, attempt, kind from bric_log where kind in ('open', 'close', 'error') and key in ('plain', 'acme') order by seq;")
    assert r.stdout == 'acme|1|open\nacme|1|close\nacme|2|open\nacme|2|close\nplain|1|error\nplain|2|open\nplain|2|error\n', r.stdout
    r = sqlite("delete from company where key = 'plain';")
    calls = len(H.calls)
    r = sqlite(SQL, SQL.replace('Resolve each', 'Resolve every'))
    assert not r.returncode and len(H.calls) == calls, r.stderr
    r = sqlite(".load ./ext/bric", "create table bare (key text primary key, parent text);", "select run('bare', 'bare shell', 'acme', 'env X=1 sh');")
    assert r.stdout == 'close\n', (r.stdout, r.stderr)
    r = sqlite("select detail ->> 'shell', (select group_concat(parent) from bare), detail ->> '$[0].key' from bric_log where job = 'bare' and kind in ('open', 'close') order by seq;")
    assert r.stdout == 'env X=1 sh|Globex|\n|Globex|acme\n', r.stdout
    r = sqlite("select json_array_length(messages), messages ->> '$[1].content[0].type', messages ->> '$[2].content[0].type', messages ->> '$[3].content[0].tool_use_id' from bric_transcript where key = 'bolt';")
    assert r.stdout == '11|server_tool_use|tool_use|c1\n', r.stdout
    assert not [d for d in os.listdir(os.environ.get('TMPDIR', '/tmp')) if d.startswith('bric.')]

    os.environ['BRIC_WORKERS'] = '1'
    r = sqlite(".load ./ext/bric",
               "insert into bric_job (source, target, brief, model, params, skills) values ('company', 'results', 'Resolve each operator to its parent.', 'cheap', '{\"thinking\": {\"type\": \"disabled\"}}', 'test/skills/*');",
               "begin; insert into company (key) values ('undo'); rollback;",
               "begin; insert into company (key) values ('dyn'), ('dyn2');", ".system sleep 1", "commit;",
               "select count(*) from bric_log where key = 'dyn';")
    assert r.stdout == '0\n', (r.returncode, r.stdout, r.stderr)
    for _ in range(100):
        time.sleep(0.2)
        r = sqlite("select key, parent from results join bric_log using (key) where key like 'dyn%' and kind = 'close' order by key;")
        if r.stdout == 'dyn|Globex\ndyn2|Globex\n':
            break
    assert r.stdout == 'dyn|Globex\ndyn2|Globex\n', r.stdout
    time.sleep(1)
    r = sqlite("select count(distinct attempt) from bric_log where key like 'dyn%';")
    assert r.stdout == '1\n', r.stdout
    # one slot: the second key is spawned by the first's exit, not left waiting for it
    r = sqlite("select group_concat(kind) from (select kind from bric_log where key like 'dyn%' and kind in ('open', 'close') order by seq);")
    assert r.stdout == 'open,close,open,close\n', r.stdout
    assert {(c['model'], c.get('thinking', {}).get('type')) for c in H.calls if keyof(c['messages'][0]['content']).startswith('dyn')} == {('cheap', 'disabled')}, H.calls[-1]
    r = sqlite("select detail ->> 'model', detail -> 'params' from bric_log where key = 'dyn' and kind = 'open';")
    assert r.stdout == 'cheap|{"thinking":{"type":"disabled"}}\n', r.stdout
    r = sqlite("select messages ->> '$[0].content' from bric_transcript where key = 'dyn';")
    assert r.stdout == '{"key":"dyn","parent":null,"source":null}\n', r.stdout
    r = sqlite("select detail ->> 'system' like '%skills are what has already been worked out%- echo: says hello (%/echo/SKILL.md)' from bric_log where key = 'dyn' and kind = 'open';")
    assert r.stdout == '1\n', r.stdout

    def wait(sql, want):
        for _ in range(100):
            time.sleep(0.2)
            r = sqlite(sql)
            if r.stdout == want:
                return
        assert r.stdout == want, r.stdout

    # a plain sqlite3, no extension: nothing runs until something loads it
    r = sqlite("create trigger chain after insert on results when new.key = 'chain' begin insert into company (key) values ('chain_next'); end;",
               "insert into company (key) values ('chain'), ('plain');")
    assert not r.returncode, r.stderr
    time.sleep(1)
    r = sqlite("select count(*) from bric_log where (key = 'chain' or key = 'plain' and attempt = 3) and kind = 'open';")
    assert r.stdout == '0\n', r.stdout
    r = sqlite(".load ./ext/bric")
    assert not r.returncode, r.stderr
    # chain_next was inserted by the model's own sqlite3 and picked up when the chain worker exited
    wait("select key from results where key like 'chain%' order by key;", 'chain\nchain_next\n')
    # plain fails every time, and stops being pending at BRIC_ATTEMPTS
    wait("select count(*) from bric_log where key = 'plain' and kind = 'error';", '3\n')
    time.sleep(1)
    r = sqlite(".load ./ext/bric", ".system sleep 1", "select count(*) from bric_log where key = 'plain' and kind in ('open', 'error') and attempt > 3;")
    assert r.stdout == '0\n', r.stdout

    print('ok')


if __name__ == '__main__':
    main()
