import json, sys, re
F = sys.argv[1]; pat = re.compile(sys.argv[2]); w = int(sys.argv[3]) if len(sys.argv) > 3 else 300
for ln, line in enumerate(open(F, encoding='utf-8', errors='replace')):
    if not pat.search(line): continue
    try: j = json.loads(line)
    except Exception: continue
    c = j.get('message', {}).get('content')
    items = c if isinstance(c, list) else [{'type': 'text', 'text': c}]
    for it in items:
        if not isinstance(it, dict): continue
        s = json.dumps(it.get('input'), ensure_ascii=False) if it.get('type') == 'tool_use' else (it.get('text') or (json.dumps(it.get('content'), ensure_ascii=False) if it.get('type') == 'tool_result' else ''))
        for m in pat.finditer(s):
            print('#%d %s %s: %s' % (ln, j.get('timestamp', '')[:19], it.get('type'), s[max(0, m.start() - w):m.end() + w].replace('\n', '\n')[:2 * w + 60]))
            break
