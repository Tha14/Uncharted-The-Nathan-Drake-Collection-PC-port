from lib import *
import ps3fp,fp
p=Pak('p3/'+sys.argv[1]+'.pak')
for mn in sys.argv[2:]:
    name,res=ps3fp.material_programs(p,mn)
    best=max(((len(fp.dis(c)),pid,c) for k,e,pid,node,c in res if k<3 and e==0))
    L=fp.dis(best[2]);print('==',name,hex(best[1]),len(L))
    # print lines up to and including the colour blend area
    for l in L[:60]:
        if re.match(r'(TEX|TXP|ADD|MAD|DIV|MUL|LRP|MOV)',l) and not re.search(r'f\[TEX[16]\], tex1[01]|tex1[2-5]',l): print('  ',l[:70])
