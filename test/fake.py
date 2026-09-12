import base64, json, os, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE = 'Acme Ltd   is a wholly owned\n\n\tsubsidiary of   Globex Corporation. ' + 'filler text. ' * 3000
QUOTE = 'wholly owned\nsubsidiary of Globex Corporation'
PNG = base64.b64encode(bytes.fromhex('89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d4944415478da63f8cfc0f01f0005000201cae1a5d90000000049454e44ae426082')).decode()


class H(BaseHTTPRequestHandler):
    calls = []

    def log_message(self, *a):
        pass

    def reply(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header('content-type', 'application/json')
        self.send_header('content-length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['content-length'])))
        if self.path == '/mcp':
            return self.mcp(req)
        H.calls.append(req)
        turn = sum(m['role'] == 'assistant' for m in req['messages'])
        key = req['messages'][0]['content']
        blocks = [c['content'] for m in req['messages'] if m['role'] == 'user' and isinstance(m['content'], list) for c in m['content']]
        results = [b[0]['text'] if isinstance(b, list) else b for b in blocks]
        if turn == 1:
            assert [x['source'] for b in blocks if isinstance(b, list) for x in b[1:]] == [{'type': 'base64', 'media_type': 'image/png', 'data': PNG}] * 2, blocks
        md = next((int(r.split(']')[0][5:]) for r in results if r.startswith('[seq ') and QUOTE in r), None)
        last = results[-1] if results else ''
        quote = 'wholly   owned\nsubsidiary of Globex Corporation'
        if turn == 0:
            content = [{'type': 'tool_use', 'id': 'c1', 'name': 'browser_navigate', 'input': {'url': 'https://example.com/' + key}},
                       {'type': 'tool_use', 'id': 'c2', 'name': 'browser_markdown', 'input': {}}]
        elif turn == 1:
            assert md and 'Navigated' in results[-2], results
            content = [{'type': 'tool_use', 'id': 'c3', 'name': 'submit', 'input': {'parent': 'Globex', 'confidence': 'certain', 'source': md, 'quote': quote}}]
        elif turn == 2:
            assert 'confidence' in last, last
            content = [{'type': 'tool_use', 'id': 'c4', 'name': 'submit', 'input': {'parent': 'Globex', 'confidence': 'high', 'source': md - 1, 'quote': quote}}]
        elif turn == 3:
            assert 'quote not found' in last, last
            content = [{'type': 'tool_use', 'id': 'c5', 'name': 'submit', 'input': {'parent': 'Globex', 'confidence': 'high', 'source': md, 'quote': quote}}]
        else:
            content = [{'type': 'text', 'text': 'giving up'}]
        if key == 'plain':
            content = [{'type': 'text', 'text': 'no idea'}]
        time.sleep(0.2)
        self.reply({'content': content, 'stop_reason': 'tool_use', 'usage': {'input_tokens': 10, 'output_tokens': 5, 'cache_read_input_tokens': 1}})

    def mcp(self, req):
        if req['method'] == 'tools/list':
            return self.reply({'jsonrpc': '2.0', 'id': req['id'], 'result': {'tools': [
                {'name': 'browser_navigate', 'description': 'go', 'inputSchema': {'type': 'object', 'properties': {'url': {'type': 'string'}}}},
                {'name': 'browser_markdown', 'description': 'read', 'inputSchema': {'type': 'object', 'properties': {}}},
                {'name': 'browser_click', 'description': 'click', 'inputSchema': {'type': 'object', 'properties': {}}}]}})
        name = req['params']['name']
        text = 'Navigated to ' + req['params']['arguments']['url'] if name == 'browser_navigate' else PAGE
        self.reply({'jsonrpc': '2.0', 'id': req['id'], 'result': {'content': [{'type': 'text', 'text': text}, {'type': 'image', 'data': PNG, 'mimeType': 'image/png'}, {'type': 'resource', 'resource': {}}]}})


SQL = '''
.load ./ext/bric
create table if not exists results (
  key text primary key,
  parent text not null,
  confidence text not null check (confidence in ('high', 'medium', 'low')),
  source integer not null,
  quote text not null
);
create trigger if not exists results_cite before insert on results
begin
  select raise(abort, 'quote not found in source ' || new.source)
   where not exists (select 1 from bric_log where seq = new.source and instr(text, squeeze(new.quote)));
end;
select run('results', 'Resolve each operator to its parent.', key) from company where key not in (select key from results);
'''


def sqlite(*sql, db='test/out.db'):
    return subprocess.run([os.environ.get('SQLITE', 'sqlite3'), db], input='\n'.join(sql), capture_output=True, text=True)


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), H)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:%d' % server.server_port
    os.environ.update(BRIC_SQLITE=os.environ.get('SQLITE', 'sqlite3'), BRIC_URL=base + '/v1/messages', BRIC_MODEL='fake', BRIC_KEY='x', BRIC_TIMEOUT='2',
                      BRIC_TOOLS=json.dumps({base + '/mcp': ['browser_navigate', 'browser_markdown']}))
    for f in ['test/out.db', 'test/out.db-wal', 'test/out.db-shm']:
        if os.path.exists(f):
            os.remove(f)
    r = sqlite('create table company (key text primary key, parent text, source integer);',
               "insert into company (key) values ('acme'), ('bolt'), ('cog'), ('plain');")
    assert not r.returncode, r.stderr
    workers = [subprocess.Popen([os.environ.get('SQLITE', 'sqlite3'), 'test/out.db'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(2)]
    outs = [w.communicate(SQL) for w in workers]
    for w, (out, err) in zip(workers, outs):
        assert not w.returncode, err
    r = sqlite('select key, parent, source, quote from results order by key;')
    assert r.stdout == ''.join('%s|Globex|%s|wholly   owned\nsubsidiary of Globex Corporation\n' % (k, sqlite(
        "select seq from bric_log where key = '%s' and kind = 'receipt' and tool = 'browser_markdown' order by seq desc limit 1;" % k).stdout.strip()) for k in ['acme', 'bolt', 'cog']), r.stdout
    r = sqlite("select key, sum(kind = 'open'), sum(kind = 'close'), sum(kind = 'error') from bric_log where kind in ('open', 'close', 'error') group by 1 order by 1;")
    assert r.stdout in ('acme|1|1|0\nbolt|1|1|0\ncog|1|1|0\nplain|%d|0|%d\n' % (n, n) for n in (1, 2)), r.stdout
    r = sqlite("select count(*) from bric_log where kind = 'receipt' and tool != 'submit' and text is null;")
    assert r.stdout == '0\n', r.stdout
    r = sqlite("select distinct detail from bric_log where key = 'plain' and kind = 'error';")
    assert r.stdout == 'reply without submission: no idea\n', r.stdout
    r = sqlite("select count(*) from bric_page where bric_page match 'globex' and rowid in (select seq from bric_log where key = 'acme' and tool = 'browser_markdown');")
    assert r.stdout == '1\n', r.stdout
    r = sqlite("select kind, input, output from bric_attempt where key = 'acme';")
    assert r.stdout == 'close|40|20\n', r.stdout
    r = sqlite("select json_array_length(detail -> 'tools'), instr(detail, 'browser_click') from bric_log where key = 'acme' and kind = 'open';")
    assert r.stdout == '5|0\n', r.stdout
    r = sqlite("select instr(text, '[resource dropped]') > 0, instr(text, '  '), instr(detail, '; 1 image ') > 0 from bric_log where key = 'acme' and kind = 'receipt' and tool = 'browser_markdown';")
    assert r.stdout == '1|0|1\n', r.stdout
    r = sqlite("delete from bric_log where key = 'plain';",
               "insert into bric_log (ts, job, key, attempt, turn, kind) values (datetime('now', '-1 hour'), 'Resolve each operator to its parent.', 'plain', 1, 3, 'call');",
               "delete from results where key = 'acme';",
               "delete from bric_log where key = 'acme' and kind = 'close';")
    r = sqlite(SQL)
    assert not r.returncode, r.stderr
    r = sqlite("select key, attempt, kind from bric_log where kind in ('open', 'close', 'error') and key in ('plain', 'acme') order by seq;")
    assert r.stdout == 'acme|1|open\nacme|1|error\nacme|2|open\nacme|2|close\nplain|1|error\nplain|2|open\nplain|2|error\n', r.stdout
    r = sqlite("delete from company where key = 'plain';")
    calls = len(H.calls)
    r = sqlite(SQL)
    assert not r.returncode and len(H.calls) == calls, r.stderr
    os.environ['BRIC_TOOLS'] = base + '/mcp'
    r = sqlite(".load ./ext/bric", "create table bare (key text primary key, parent text);", "select run('bare', 'bare url', 'acme');")
    assert not r.returncode, r.stderr
    r = sqlite("select json_array_length(detail -> 'tools') from bric_log where job = 'bare url' and kind = 'open';")
    assert r.stdout == '6\n', r.stdout
    r = sqlite("select json_array_length(messages), messages ->> '$[1].content[0].type', messages ->> '$[2].content[0].tool_use_id' from bric_transcript where key = 'bolt';")
    assert r.stdout == '8|tool_use|c1\n', r.stdout

    os.environ['BRIC_WORKERS'] = '1'
    r = sqlite(".load ./ext/bric",
               "select job('company', 'results', 'Resolve each operator to its parent.') is null;",
               "begin; insert into company (key) values ('dyn'), ('dyn2'); commit;",
               "select count(*) from bric_log where key = 'dyn';")
    assert r.stdout == '1\n0\n', (r.returncode, r.stdout, r.stderr)
    for _ in range(100):
        time.sleep(0.2)
        r = sqlite("select key, parent from results where key like 'dyn%' order by key;")
        if r.stdout == 'dyn|Globex\ndyn2|Globex\n':
            break
    assert r.stdout == 'dyn|Globex\ndyn2|Globex\n', r.stdout
    r = sqlite("select count(distinct attempt) from bric_log where key like 'dyn%';")
    assert r.stdout == '1\n', r.stdout

    print('ok')


if __name__ == '__main__':
    main()
