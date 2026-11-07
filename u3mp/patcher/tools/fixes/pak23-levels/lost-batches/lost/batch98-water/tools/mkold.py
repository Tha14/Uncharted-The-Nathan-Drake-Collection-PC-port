import json,glob,sys
S='/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad'
R=json.load(open(S+'/nL2/plans/replan.json'))
for lv in sys.argv[1:]:
    a=json.load(open(S+'/nL2/plans/%s.plan.json'%lv))
    b=json.load(open(glob.glob('/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/maps/req7/*/%s.plan.json'%lv)[0]))
    for e in R[lv]:
        o=b['mats'].get(e['mat'])
        assert o and o['key']==e['old'], (lv,e['mat'],o and o['key'])
        a['mats'][e['mat']]=o
    json.dump(a,open(S+'/h006/plans_old/%s.plan.json'%lv,'w')); print(lv,'ok')
