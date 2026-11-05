import json, sys, os
ROOT = 'C:/Users/jonas/.claude/projects/C--Users-jonas-Documents-ChatGPT-test/'
f, name, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(outdir, exist_ok=True); k = 0
base = name.split('/')[-1]
for ln, line in enumerate(open(ROOT + f, encoding='utf-8', errors='replace')):
    if base not in line: continue
    j = json.loads(line); c = j.get('message', {}).get('content')
    if not isinstance(c, list): continue
    for it in c:
        if it.get('type') != 'tool_use': continue
        inp = it.get('input', {}); fp = (inp.get('file_path') or '').replace(chr(92), '/')
        kind = it['name']; t = j.get('timestamp', '')
        if fp.endswith(name):
            k += 1
            body = inp.get('content') if kind == 'Write' else json.dumps({x: inp.get(x) for x in ('old_string', 'new_string', 'replace_all')})
            open(outdir + '/%03d_%s_%s.txt' % (k, kind, t.replace(':', '')), 'w', encoding='utf-8', newline='\n').write(body or '')
            print(k, ln, t, kind, len(body or ''))
        elif kind == 'Bash':
            cmd = inp.get('command', '')
            if base in cmd and ('sed -i' in cmd or 'cat > ' + base in cmd or "open('" + base in cmd or ('dctools/' + base) in cmd and ('write' in cmd or 'replace' in cmd)):
                k += 1
                open(outdir + '/%03d_Bash_%s.txt' % (k, t.replace(':', '')), 'w', encoding='utf-8', newline='\n').write(cmd)
                print(k, ln, t, 'Bash-maybe-edit', len(cmd), cmd[:150].replace('\n', ' '))
