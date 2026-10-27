#!/usr/bin/env python3
"""Summarize 10..50 s of receiver_ab_browser.js / remote probe reports.

Counters, callback coverage and decode cadence are distinct. No scan-out or
glass-to-glass latency is inferred. Usage: receiver_ab_analyze.py REPORT...
"""
import json
import math
import statistics
import sys


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return dict(count=len(values), mean_ms=statistics.mean(values), stddev_ms=statistics.pstdev(values),
                p95_ms=values[math.ceil(len(values)*.95)-1], p99_ms=values[math.ceil(len(values)*.99)-1], max_ms=values[-1])


def endpoints(samples, start, clock):
    assert samples and samples[-1][clock] - start >= 50000, 'run did not cover the analysis window'
    return [min(samples, key=lambda s: abs(s[clock]-start-t)) for t in [10000, 50000]]


def browser(data):
    assert not data['errors'], 'exclude an interrupted browser run'
    first, last = endpoints(data['samples'], data['start'], 'at')
    seconds = (last['at']-first['at'])/1000
    before = {s['id']:s for s in first['stats']}
    tracks=[]
    for s in last['stats']:
        if s['type'] != 'inbound-rtp' or s.get('kind') != 'video': continue
        p=before[s['id']]
        delta=lambda key: s[key]-p[key]
        n=delta('framesDecoded'); emitted=delta('jitterBufferEmittedCount')
        mean=delta('totalInterFrameDelay')/n
        tracks.append(dict(mid=s.get('mid'), decoded_fps=n/seconds, decoded_frames=n,
            decode_gap_mean_ms=mean*1000,
            decode_gap_stddev_ms=math.sqrt(max(0,delta('totalSquaredInterFrameDelay')/n-mean*mean))*1000,
            decode_mean_ms=delta('totalDecodeTime')/n*1000,
            jitter_buffer_mean_ms=delta('jitterBufferDelay')/emitted*1000,
            jitter_target_mean_ms=delta('jitterBufferTargetDelay')/emitted*1000,
            dropped_delta=delta('framesDropped'), lost_delta=delta('packetsLost'), freeze_delta=delta('freezeCount'),
            decoder_implementation=s.get('decoderImplementation'), hardware_decoder=s.get('powerEfficientDecoder')))
    displayed=[]
    for t in data['tracks']:
        frames=[f for f in t['frames'] if 10000 <= f['now']-data['start'] <= 50000]
        assert len(frames)>1
        count=frames[-1]['presentedFrames']-frames[0]['presentedFrames']
        duration=(frames[-1]['presentationTime']-frames[0]['presentationTime'])/1000
        displayed.append(dict(presented_counter_fps=count/duration, callback_count=len(frames),
            callback_coverage=(len(frames)-1)/count,
            observed_presentation_gap_ms=distribution([b['presentationTime']-a['presentationTime'] for a,b in zip(frames,frames[1:])]),
            receive_to_presentation_ms=distribution([f['presentationTime']-f['receiveTime'] for f in frames if 'receiveTime' in f]),
            note='RVFC can skip frames; gaps are observations, not proof of dropped frames'))
    return dict(kind='browser', seconds=seconds, tracks=tracks, display=displayed,
                selected_pair=last['metrics']['selected_candidate_pair'], visibility=data['visibility'],
                long_tasks=len(data['longTasks']), errors=data['errors'])


def native(data):
    assert data['passed'], 'exclude a failed native run'
    start=data['measurement_start_clock_ms']; first,last=endpoints(data['samples'],start,'clock_ms')
    seconds=(last['clock_ms']-first['clock_ms'])/1000
    tracks=[]
    for t in data['receiver']['tracks']:
        assert t['decoded_frames'] <= t['rtp_unique_frame_timestamps'], 'decoded frame identity anomaly'
        p=next(v for v in first['tracks'] if v['pad']==t['pad'])
        s=next(v for v in last['tracks'] if v['pad']==t['pad'])
        events=[f for f in t['decoded_events_pts_ns_clock_ms'] if 10000 <= f[1]-start <= 50000]
        drawn=[f for f in t['draw_events_pts_ns_clock_ms'] if 10000 <= f[1]-start <= 50000]
        decode_gaps=distribution([b[1]-a[1] for a,b in zip(events,events[1:])])
        decoded=dict(t['decoded_events_pts_ns_clock_ms'])
        tracks.append(dict(pad=t['pad'], decoded_fps=(s['decoded']-p['decoded'])/seconds,
            decoded_frames=s['decoded']-p['decoded'], decode_gap_mean_ms=decode_gaps['mean_ms'],
            decode_gap_stddev_ms=decode_gaps['stddev_ms'], decode_gap=decode_gaps,
            depay_to_decode_mean_ms=(s['decode_sum_ms']-p['decode_sum_ms'])/(s['decode_count']-p['decode_count']),
            draw_fps=(len(drawn)-1)*1000/(drawn[-1][1]-drawn[0][1]),
            draw_gap=distribution([b[1]-a[1] for a,b in zip(drawn,drawn[1:])]),
            decode_to_draw=distribution([at-decoded[pts] for pts,at in drawn if pts in decoded]),
            whole_run_rtp_frames=t['rtp_unique_frame_timestamps'], whole_run_decoded_frames=t['decoded_frames'],
            duplicate_pts=t['duplicate_decoded_pts']))
    rtp=[s for s in last['webrtc'].values() if s.get('type')=='inbound-rtp']
    return dict(kind='native', seconds=seconds, passed=data['passed'], tracks=tracks,
                selected_pair=data['receiver']['selected_pair'],
                packet_loss=[s.get('packets-lost') for s in rtp],
                jitterbuffer=[s.get('gst-rtpjitterbuffer-stats') for s in rtp],
                note='GL draw callback is not scan-out; depay-to-decode includes parsing and differs from browser decodeTime')


if __name__ == '__main__':
    result={}
    for path in sys.argv[1:]:
        with open(path) as f: data=json.load(f)
        result[path.rsplit('/',1)[-1]] = browser(data) if data['schema']=='browser-receiver-ab-v1' else native(data)
    print(json.dumps(result,indent=2))
