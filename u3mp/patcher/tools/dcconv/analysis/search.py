import os,sys,json,re
ROOT="C:/Users/jonas/.claude/projects/C--Users-jonas-Documents-ChatGPT-test/"
PAT=re.compile(r'class DCEdit|def add_symbols|def copy_words|def add_string|class DC\b|class DC:|dcsyms\.py|common\.py|dcedit\.py')
out=open(sys.argv[1],'w',encoding='utf-8')
files=[]
for dp,dn,fn in os.walk(ROOT):
  for f in fn:
    if f.endswith('.jsonl'):files.append(os.path.join(dp,f))
for f in files:
  with open(f,encoding='utf-8',errors='replace') as fh:
    for ln,line in enumerate(fh):
      if 'DCEdit' not in line and 'dcsyms' not in line and 'common.py' not in line and 'dcedit' not in line: continue
      try: j=json.loads(line)
      except Exception: continue
      ts=j.get('timestamp','')
      msg=j.get('message',{})
      cont=msg.get('content') if isinstance(msg,dict) else None
      items=cont if isinstance(cont,list) else [{'type':'text','text':cont}] if isinstance(cont,str) else []
      tur=j.get('toolUseResult')
      for it in items:
        txt=''
        if it.get('type')=='tool_use': txt=json.dumps(it.get('input'),ensure_ascii=False)
        elif it.get('type')=='tool_result':
          c=it.get('content'); txt=c if isinstance(c,str) else json.dumps(c,ensure_ascii=False)
        elif it.get('type')=='text': txt=it.get('text') or ''
        if PAT.search(txt):
          out.write('%s\t%s\t%d\t%s\t%d\t%s\n'%(os.path.relpath(f,ROOT),ts,ln,it.get('type'),len(txt),txt[:600].replace('\n','\n').replace('\t',' ')))
out.close()
