import sys,json
f=sys.argv[1];want=set(int(x) for x in sys.argv[2].split(','))
ROOT="C:/Users/jonas/.claude/projects/C--Users-jonas-Documents-ChatGPT-test/"
with open(ROOT+f,encoding='utf-8',errors='replace') as fh:
  for ln,line in enumerate(fh):
    if ln in want:
      j=json.loads(line);msg=j.get('message',{});c=msg.get('content')
      print('#### line',ln,j.get('timestamp'))
      for it in (c if isinstance(c,list) else [{'type':'text','text':c}]):
        if it.get('type')=='tool_use':print('[tool_use]',json.dumps(it.get('input'),ensure_ascii=False,indent=0)[:20000])
        elif it.get('type')=='tool_result':
          cc=it.get('content');print('[tool_result]',cc if isinstance(cc,str) else '\n'.join(x.get('text','') for x in cc if isinstance(x,dict)))
        elif it.get('type')=='text':print('[text]',it.get('text'))
