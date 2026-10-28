#!/usr/bin/env python3
"""Bounded, opt-in functional codec diagnostics. No cameras or network are opened.

Each explicitly ordered candidate runs in a fresh process. A pass proves actual
encoding and decoding at these settings, not hardware rendering or WAN latency.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

ENCODERS = ('cpu', 'nvenc', 'vaapi')
DECODERS = ('avdec_h264', 'avdec_h265', 'vtdec_hw', 'nvv4l2decoder',
            'vah264dec', 'vah265dec', 'vaapih264dec', 'vaapih265dec')


def attempt(worker, settings, timeout_ms):
    started = time.monotonic()
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        process = subprocess.Popen([str(worker), *(str(settings[k]) for k in
            ('backend', 'codec', 'decoder', 'width', 'height', 'fps', 'bitrate_kbps', 'gop'))],
            stdout=stdout, stderr=stderr, start_new_session=True)
        result = dict(settings, passed=False, timeout=False)
        try:
            process.wait(timeout=timeout_ms / 1000)
        except subprocess.TimeoutExpired:
            result.update(timeout=True, error='whole-process deadline exceeded, including teardown')
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                result['fatal_unreaped_worker'] = True
        result.update(exit_code=process.returncode, elapsed_ms=(time.monotonic()-started)*1000)
        stdout.seek(0)
        raw = stdout.read(65537)
        stderr.seek(0, os.SEEK_END)
        stderr.seek(max(0, stderr.tell()-4096))
        result['stderr_tail'] = stderr.read().decode('utf-8', errors='replace')
        if not result['timeout']:
            try:
                if len(raw) > 65536:
                    raise ValueError('worker JSON exceeds bounded output size')
                report = json.loads(raw)
                result['worker'] = report
                if process.returncode != 0 or report.get('passed') is not True:
                    raise ValueError(report.get('error', 'worker did not pass'))
                if report.get('schema') != 'codec-roundtrip-v1' or any(report.get(k) != v for k, v in settings.items()):
                    raise ValueError('worker configuration identity mismatch')
                if (report.get('initial_idr_valid') is not True or report.get('decoded_frames', 0) < 1
                    or report.get('decoded_pts_ns') != report.get('encoded_running_pts_ns')
                    or type(report.get('decoded_pts_ns')) is not int or report['decoded_pts_ns'] < 0
                    or report.get('encoder_factory_available') is not True
                    or report.get('decoder_factory_available') is not True):
                    raise ValueError('worker lacks actual IDR/decode/PTS evidence')
                result['passed'] = True
            except (ValueError, TypeError, AttributeError) as error:
                result['error'] = str(error)
        return result


def run(worker, encoders, decoders, settings, timeout_ms):
    attempts = []
    for encoder in encoders:
        for decoder in decoders:
            result = attempt(worker, dict(settings, backend=encoder, decoder=decoder), timeout_ms)
            attempts.append(result)
            if result['passed'] or result.get('fatal_unreaped_worker'):
                return dict(schema='codec-selection-v1', passed=result['passed'], attempts=attempts,
                            selected=dict(backend=encoder, decoder=decoder) if result['passed'] else None)
    return dict(schema='codec-selection-v1', passed=False, selected=None, attempts=attempts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--encoders', default='cpu')
    parser.add_argument('--decoders', default='avdec_h264')
    parser.add_argument('--codec', choices=('h264', 'h265'), default='h264')
    for key, default in [('width',128), ('height',96), ('fps',20), ('bitrate-kbps',125), ('gop',30), ('timeout-ms',10000)]:
        parser.add_argument('--'+key, type=int, default=default)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if os.name != 'posix':
        parser.error('codec worker isolation currently requires Linux or macOS process groups')
    encoders, decoders = args.encoders.split(','), args.decoders.split(',')
    if (len(encoders)>3 or len(decoders)>4 or len(set(encoders))!=len(encoders) or len(set(decoders))!=len(decoders)
        or any(x not in ENCODERS for x in encoders) or any(x not in DECODERS for x in decoders)):
        parser.error('use distinct supported encoder/decoder names (at most 3 encoders / 4 decoders)')
    for key, maximum in [('width',4096),('height',4096),('fps',120),('bitrate_kbps',100000),('gop',240),('timeout_ms',60000)]:
        if not 0 < getattr(args,key) <= maximum:
            parser.error(key+' outside diagnostic bounds')
    if args.width % 2 or args.height % 2:
        parser.error('NV12 dimensions must be even')
    if args.output and args.output.exists():
        parser.error('output exists; evidence is never overwritten')
    result = run(args.probe.resolve(strict=True), encoders, decoders,
        {k:getattr(args,k) for k in ('codec','width','height','fps','bitrate_kbps','gop')},args.timeout_ms)
    text = json.dumps(result, indent=2)+'\n'
    if args.output:
        with args.output.open('x') as stream:
            stream.write(text)
    print(text,end='')
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
