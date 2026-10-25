#!/usr/bin/env python3
"""Report brush/mesh/npc/light counts + bounding box for a World.ozone file."""
import sys, re, os

def report(path):
    brushes=0; meshes=0; npcs=0; lights=0; pickups=0; zones=0
    xs=[]; ys=[]; zs=[]
    with open(path) as f:
        for line in f:
            s=line.strip()
            if not s or s.startswith('#'): continue
            toks=s.split()
            first=toks[0].lower()
            if first in ('add','sub','intersect','deresc','box','cyl','sph','pyr','pln'):
                brushes+=1
            elif first.startswith('mesh.'):
                meshes+=1
            elif first=='npc':
                npcs+=1
            elif first=='light':
                lights+=1
            elif first=='pickup':
                pickups+=1
            elif first=='zone':
                zones+=1
            # collect numeric args for bbox (best effort)
            for t in toks[1:]:
                try:
                    v=float(t)
                except ValueError:
                    if '=' in t:
                        try: v=float(t.split('=',1)[1])
                        except: continue
                    else: continue
                # heuristic: ignore tiny counts; just collect all
            # simpler: parse trailing floats
            nums=[]
            for t in toks[1:]:
                tt=t.split('=',1)[-1]
                try: nums.append(float(tt))
                except: pass
            # If line has >=3 trailing floats, treat first 3 as position-ish
            if len(nums)>=3 and first not in ('add','sub','intersect','deresc'):
                xs.append(nums[0]); ys.append(nums[1]); zs.append(nums[2])
    print(f"World: {path}")
    print(f"  brushes={brushes} meshes={meshes} npcs={npcs} lights={lights} pickups={pickups} zones={zones}")
    if xs:
        print(f"  bbox x=[{min(xs):.1f},{max(xs):.1f}] y=[{min(ys):.1f},{max(ys):.1f}] z=[{min(zs):.1f},{max(zs):.1f}]")

if __name__=='__main__':
    for p in sys.argv[1:]:
        report(p)