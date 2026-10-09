# an anthropic messages endpoint over an openai chat completions one, for
# models with no anthropic route of their own (mistral). server tools are
# dropped, so the web skill searches without one. usage:
#   OPENAI_URL=https://api.mistral.ai/v1/chat/completions \
#   OPENAI_PARAMS='{"reasoning_effort":"none"}' python3 test/openai.py 8787
# then BRIC_URL=http://127.0.0.1:8787 and BRIC_KEY is the provider's key
import json, os, sys, urllib.request, urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

URL, EXTRA = os.environ['OPENAI_URL'], json.loads(
    os.environ.get('OPENAI_PARAMS', '{}'))


def text(c):
    return c if isinstance(c, str) else '\n'.join(
        b.get('text', '') for b in c if b['type'] == 'text')


def image(b):
    s = b['source']
    return {'type': 'image_url',
            'image_url': 'data:%s;base64,%s' % (s['media_type'], s['data'])}


def messages(req):
    out = [{'role': 'system', 'content': text(req['system'])}]
    for m in req['messages']:
        c = m['content']
        if isinstance(c, str):
            out.append({'role': m['role'], 'content': c})
        elif m['role'] == 'assistant':
            calls = [{'id': b['id'], 'type': 'function', 'function': {
                'name': b['name'], 'arguments': json.dumps(b['input'])}}
                for b in c if b['type'] == 'tool_use']
            # mistral refuses an assistant turn with neither
            out.append({'role': 'assistant',
                        'content': text(c) or ('' if calls else '.'),
                        **({'tool_calls': calls} if calls else {})})
        else:
            pics = []
            for b in c:
                if b['type'] == 'tool_result':
                    r = b.get('content', '')
                    r = [{'type': 'text', 'text': r}] if isinstance(r, str) else r
                    pics += [image(x) for x in r if x['type'] == 'image']
                    out.append({'role': 'tool', 'tool_call_id': b['tool_use_id'],
                                'content': text(r) or '(no output)'})
                elif b['type'] == 'image':
                    pics.append(image(b))
                elif b['type'] == 'text':
                    pics.append({'type': 'text', 'text': b['text']})
            if pics:
                out.append({'role': 'user', 'content': pics})
    return out


def translate(req):
    tools = [{'type': 'function', 'function': {
        'name': t['name'], 'description': t.get('description', ''),
        'parameters': t['input_schema']}}
        for t in req.get('tools', []) if 'input_schema' in t]
    return {'model': req['model'], 'max_tokens': req['max_tokens'],
            'messages': messages(req), **({'tools': tools} if tools else {}),
            **EXTRA}


def back(r):
    m, u = r['choices'][0]['message'], r.get('usage', {})
    c = m.get('content') or ''
    c = c if isinstance(c, str) else text(
        [b for b in c if b['type'] == 'text'])
    blocks = [{'type': 'text', 'text': c}] if c else []
    for t in m.get('tool_calls') or []:
        try:
            a = json.loads(t['function']['arguments'] or '{}')
        except ValueError:
            a = {}
        blocks.append({'type': 'tool_use', 'id': t['id'],
                       'name': t['function']['name'], 'input': a})
    return {'type': 'message', 'role': 'assistant', 'content': blocks,
            'stop_reason': 'tool_use' if m.get('tool_calls') else 'end_turn',
            'usage': {'input_tokens': u.get('prompt_tokens', 0),
                      'output_tokens': u.get('completion_tokens', 0)}}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers['content-length'])))
        q = urllib.request.Request(URL, json.dumps(translate(req)).encode(), {
            'content-type': 'application/json',
            'authorization': 'Bearer ' + self.headers['x-api-key']})
        try:
            with urllib.request.urlopen(q, timeout=300) as r:
                status, body = 200, json.dumps(back(json.load(r))).encode()
        except urllib.error.HTTPError as e:
            status, body = e.code, e.read()
        except Exception as e:
            status, body = 502, repr(e).encode()
        self.send_response(status)
        self.send_header('content-type', 'application/json')
        self.send_header('content-length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
