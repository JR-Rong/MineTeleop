import argparse
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--output',required=True);a=p.parse_args();s=Path(a.source).read_text();out=[]
for name in ['brake_direct_authority_locked','mark_direct_authority_worker_failed','direct_authority_loop','tick_control_service']:
 needle='  void '+name+'(';start=s.find(needle)
 if start<0:continue
 depth=0
 for i in range(s.index('{',start),len(s)):
  if s[i]=='{':depth+=1
  elif s[i]=='}':
   depth-=1
   if not depth:out.append(s[start:i+1]);break
Path(a.output).write_text('\n'.join(out)+'\n')
