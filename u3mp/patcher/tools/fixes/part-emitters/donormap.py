"""Generic fixed-size struct converter: learned PS3 word -> PS4 word map (sw) + donor for everything else."""
import collections
class DonorMap:
    def __init__(self,samples,minc=0.99):
        # samples: list of (w3 list, w4 list), same lengths per class
        self.samples=samples
        votes=collections.defaultdict(collections.Counter); nz=collections.Counter()
        for w3,w4 in samples:
            for i,v in enumerate(w3):
                if v==b'\0\0\0\0': continue
                nz[i]+=1
                for j,u in enumerate(w4):
                    if u==v[::-1]: votes[i][j]+=1
        self.map={}
        for i in nz:
            if votes[i]:
                j,c=votes[i].most_common(1)[0]
                if c>=minc*nz[i]: self.map[i]=j
        # PS4 positions written by the map
        self.cover=set(self.map.values())
    def convert(self,w3,exclude=None):
        un=[i for i in range(len(w3)) if i not in self.map]
        best=None
        for idx,(d3,d4) in enumerate(self.samples):
            if exclude is not None and idx==exclude: continue
            if len(d3)!=len(w3): continue
            diff=sum(1 for i in un if d3[i]!=w3[i])
            if best is None or diff<best[0]:
                best=(diff,idx)
                if diff==0: break
        if best is None: return None,None
        d3,d4=self.samples[best[1]]
        out=list(d4)
        # zero-valued PS3 words that are mapped: write zeros too (map learned from nonzero only)
        for i,j in self.map.items(): out[j]=w3[i][::-1]
        return out,best[0]
