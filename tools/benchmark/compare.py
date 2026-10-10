#!/usr/bin/env python3
"""Join optical, paired frame observations; never subtract two percentiles.

Each JSONL record: run, scenario, source_frame_id (optical test pattern identity),
variant (six/two-partition/two-bev-720p/two-bev-540p), path (drive/bev),
optical_latency_ms, uncertainty_ms. Network samples use event=network, variant,
run, at_seconds, vehicle_tx_bytes, cloud_media_tx_bytes, encoder_video_bytes,
cpu_percent, rss_bytes. Warmup is excluded; counters must not reset mid-run.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

VARIANTS=('six','two-partition','two-bev-720p','two-bev-540p')
SCENARIOS={'static','moving','texture'}

def percentile(values,q=.95):
    if not values:raise ValueError('empty measurement set')
    return sorted(values)[math.ceil(q*len(values))-1]

def compare(rows):
    frames={};network={};unqualified=0
    for row in rows:
        variant=row['variant'];run=row['run']
        if variant not in VARIANTS or run not in (1,2,3):raise ValueError('unknown variant/run')
        if row.get('event')=='network':network.setdefault((variant,run),[]).append(row);continue
        if row['scenario'] not in SCENARIOS or row['path'] not in ('drive','bev'):raise ValueError('unknown scenario/path')
        for key in ('optical_latency_ms','uncertainty_ms'):
            if not math.isfinite(row[key]) or row[key]<0:raise ValueError('invalid optical measurement')
        if row['uncertainty_ms']>5:unqualified+=1;continue
        key=(run,row['scenario'],row['source_frame_id'],row['path'])
        per=frames.setdefault(key,{})
        if variant in per:raise ValueError('duplicate frame identity; reopen/drop/reorder must retain distinct IDs')
        per[variant]=row['optical_latency_ms']
    report={'schema':1,'measured':True,'optical_excluded_uncertainty_gt_5ms':unqualified,'variants':{}}
    for variant in VARIANTS:
        pairs={};missing=0
        for key,samples in frames.items():
            if variant not in samples or 'six' not in samples:missing+=1;continue
            pairs.setdefault(key[3],[]).append(samples[variant]-samples['six'])
        runs=[]
        for run in (1,2,3):
            samples=sorted(network.get((variant,run),[]),key=lambda r:r['at_seconds'])
            samples=[r for r in samples if r['at_seconds']>=30]
            if len(samples)<2 or samples[-1]['at_seconds']-samples[0]['at_seconds']<600:raise ValueError(f'{variant}/{run}: need 30 s warmup and 600 s measured counters')
            duration=samples[-1]['at_seconds']-samples[0]['at_seconds'];rates={}
            for counter in ('vehicle_tx_bytes','cloud_media_tx_bytes','encoder_video_bytes'):
                values=[r[counter] for r in samples]
                if any(b<a for a,b in zip(values,values[1:])):raise ValueError('counter reset; start a new run')
                rates[counter.replace('_bytes','_bps')]=(values[-1]-values[0])*8/duration
            rates.update(run=run,seconds=duration,cpu_mean=sum(r['cpu_percent'] for r in samples)/len(samples),rss_peak=max(r['rss_bytes'] for r in samples));runs.append(rates)
        if variant!='six' and not all(any(k[0]==run and k[1]==scenario and variant in v and 'six' in v for k,v in frames.items()) for run in (1,2,3) for scenario in SCENARIOS):raise ValueError('paired measurements must cover every run and scenario')
        report['variants'][variant]={'network_runs':runs,'paired_extra_latency':{path:{'samples':len(values),'p95_ms':percentile(values),'max_ms':max(values)} for path,values in pairs.items()},'unpaired_identities':missing}
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('jsonl');parser.add_argument('--out',required=True);args=parser.parse_args()
    raw=Path(args.jsonl).read_bytes();rows=[json.loads(line) for line in raw.splitlines() if line.strip()]
    result=compare(rows);result['source_sha256']=hashlib.sha256(raw).hexdigest();Path(args.out).write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
if __name__=='__main__':main()
