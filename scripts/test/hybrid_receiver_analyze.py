#!/usr/bin/env python3
"""Compare fixed UI load phases, preserving endpoint and sampling differences.

Usage: hybrid_receiver_analyze.py RUN_DIR...
Each run contains ui.json, metadata.json and (for native) native.json.
No receiver-local duration is labelled glass-to-glass or scan-out latency.
"""
import json
import math
import pathlib
import statistics
import sys

from receiver_ab_analyze import distribution

PHASES = ('idle', 'ui8', 'ui40', 'stall200', 'recovery')


def select_window(samples, start, low, high, key):
    before = min(samples, key=lambda row: abs(row[key] - start - low))
    after = min(samples, key=lambda row: abs(row[key] - start - high))
    assert abs(before[key] - start - low) <= 2000 and abs(after[key] - start - high) <= 2000, 'sample coverage gap'
    assert after[key] > before[key], 'empty measurement window'
    return before, after, (after[key] - before[key]) / 1000


def presentation_summary(frames):
    assert len(frames) > 1, 'insufficient presentation observations'
    delta = frames[-1]['presentedFrames'] - frames[0]['presentedFrames']
    seconds = (frames[-1]['presentationTime'] - frames[0]['presentationTime']) / 1000
    assert delta > 0 and seconds > 0, 'presentation counter reset'
    return dict(presented_fps=delta / seconds, observed_frames=len(frames),
                callback_coverage=(len(frames)-1) / delta,
                observed_receive_to_presentation=distribution([
                    frame['presentationTime']-frame['receiveTime'] for frame in frames
                    if isinstance(frame.get('receiveTime'), (int, float)) and frame['presentationTime'] >= frame['receiveTime']]),
                note='RVFC samples can miss presentations under UI load; observed tails are not a complete frame population')


def browser_window(data, low, high):
    first, last, seconds = select_window(data['samples'], data['start'], low, high, 'at')
    initial = {row['id']: row for row in first['stats']}
    result = []
    for row in last['stats']:
        if row['type'] != 'inbound-rtp' or row.get('kind') != 'video':
            continue
        old = initial[row['id']]
        delta = lambda name: row[name]-old[name]
        frames = delta('framesDecoded')
        assert frames > 0, 'no decoded frames'
        mean = delta('totalInterFrameDelay')/frames
        result.append(dict(mid=row.get('mid'), decoded_fps=frames/seconds,
            decode_gap_mean_ms=mean*1000,
            decode_gap_stddev_ms=math.sqrt(max(0, delta('totalSquaredInterFrameDelay')/frames-mean**2))*1000,
            receive_processing_mean_ms=delta('totalProcessingDelay')/frames*1000,
            decode_mean_ms=delta('totalDecodeTime')/frames*1000,
            jitter_mean_ms=delta('jitterBufferDelay')/delta('jitterBufferEmittedCount')*1000,
            freeze_count=delta('freezeCount'), freeze_duration_s=delta('totalFreezesDuration'),
            frames_dropped=delta('framesDropped'), packets_lost=delta('packetsLost')))
    presentation = []
    for track in data['tracks']:
        frames = [f for f in track['frames'] if low <= f['presentationTime']-data['start'] <= high]
        presentation.append(dict(camera=track['camera'], **presentation_summary(frames)))
    return dict(seconds=seconds, tracks=result, presentation=presentation)


def native_window(data, report, low, high):
    start = data['nativeStart']
    first, last, seconds = select_window(report['samples'], start, low, high, 'clock_ms')
    result = []
    for track in report['receiver']['tracks']:
        assert track['identity_observation_overflow'] == 0, 'frame observation truncated'
        assert track['decoded_frames'] <= track['rtp_unique_frame_timestamps'], 'frame identity anomaly'
        decoded = {pts: at for pts, at in track['decoded_events_pts_ns_clock_ms']}
        received = {pts: at for pts, at in track['received_events_pts_ns_clock_ms']}
        events = [(pts, at) for pts, at in decoded.items() if low <= at-start <= high]
        drawn = [(pts, at) for pts, at in track['draw_events_pts_ns_clock_ms'] if low <= at-start <= high]
        assert len(events)>1 and len(drawn)>1, 'no native output'
        gaps = distribution([b[1]-a[1] for a, b in zip(events, events[1:])])
        draw_gaps = [b[1]-a[1] for a, b in zip(drawn, drawn[1:])]
        before = next(t for t in first['tracks'] if t['pad'] == track['pad'])
        after = next(t for t in last['tracks'] if t['pad'] == track['pad'])
        result.append(dict(pad=track['pad'], decoded_fps=(after['decoded']-before['decoded'])/seconds,
            decode_gap=gaps, decode_gap_stddev_ms=gaps['stddev_ms'],
            drawn_fps=(len(drawn)-1)*1000/(drawn[-1][1]-drawn[0][1]), draw_gap=distribution(draw_gaps),
            draw_gaps_over_100ms=sum(v>100 for v in draw_gaps), draw_gaps_over_250ms=sum(v>250 for v in draw_gaps),
            decode_to_draw=distribution([at-decoded[pts] for pts, at in drawn if pts in decoded]),
            jitter_input_to_decode=distribution([at-received[pts] for pts, at in events if pts in received]),
            jitter_input_to_draw=distribution([at-received[pts] for pts, at in drawn if pts in received]),
            arrival_match_coverage=sum(pts in received for pts, _at in drawn)/len(drawn)))
    return dict(seconds=seconds, tracks=result)


def analyze(directory):
    directory = pathlib.Path(directory)
    data = json.loads((directory/'ui.json').read_text())
    metadata = json.loads((directory/'metadata.json').read_text())
    assert not data['errors'] and not metadata['errors'], 'interrupted/failed run'
    assert data.get('overflow',0) == 0, 'truncated UI evidence'
    assert data['end']-data['start'] >= data['seconds']*1000-1000, 'run duration incomplete'
    assert not any(e['hidden'] for e in data['visibility']), 'page was hidden'
    report = json.loads((directory/'native.json').read_text()) if data['mode']=='native' else None
    if report:
        assert report['passed'] and not report.get('error'), 'native receiver failed'
    phases = {}
    for index, phase in enumerate(PHASES):
        # Exclude five seconds around each planned phase transition, not tails
        # inside a phase. Original per-frame/RTCStats evidence is retained.
        low = index*data['seconds']*200 + 5000
        high = (index+1)*data['seconds']*200 - 5000
        assert high > low, 'use runs longer than 50 seconds for phase analysis'
        samples = [s for s in data['load'] if low <= s['at']-data['start'] <= high]
        assert samples, 'missing UI load evidence'
        result = native_window(data, report, low, high) if report else browser_window(data, low, high)
        result['ui_load'] = dict(callbacks=len(samples), requested_ms=sum(s['requested_ms'] for s in samples),
            actual_ms=sum(s['actual_ms'] for s in samples), window_ms=high-low,
            busy_fraction=sum(s['actual_ms'] for s in samples)/(high-low))
        phases[phase] = result
    return dict(mode=data['mode'], duration_s=(data['end']-data['start'])/1000, phases=phases,
        selected_pair=report['receiver']['selected_pair'] if report else data['samples'][-1].get('metrics',{}).get('selected_candidate_pair'),
        limitations=['Native endpoint is GL draw, browser endpoint is compositor submission; neither is scan-out.',
          'Native arrival is jitterbuffer input after SRTP; browser receiveTime is WebRTC frame receive metadata.',
          'Use within-client load-versus-idle differences; absolute path comparisons retain these endpoint differences.',
          'Native buffering target 30 ms versus browser adaptive buffering; not a renderer-only experiment.'])


if __name__ == '__main__':
    print(json.dumps({pathlib.Path(p).name: analyze(p) for p in sys.argv[1:]}, indent=2))
