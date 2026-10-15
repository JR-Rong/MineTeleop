"""Execute the unchanged vehicle readiness observer without GStreamer I/O."""
import argparse
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--source',required=True);p.add_argument('--output',required=True);a=p.parse_args()
s=Path(a.source).read_text()
def extract(anchor):
    start=s.index(anchor);depth=0
    for index in range(s.index('{',start),len(s)):
        if s[index]=='{':depth+=1
        elif s[index]=='}':
            depth-=1
            if depth==0:return s[start:index+1]+'\n'
    raise RuntimeError('recovery enforcer method incomplete: '+anchor)
# Both bodies are unchanged. The small freshness helper becomes a fixture
# member so this no-GStreamer target does not link the whole vehicle runtime.
Path(a.output).write_text(extract('bool camera_encoded_frame_fresh(')+extract('  void enforce_control_recovery() {'))
