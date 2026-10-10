#!/usr/bin/env python3
"""Synthetic/golden validation. Passing here grants no driving qualification."""
import importlib.util
import importlib.machinery
import json
import math
import tempfile
from pathlib import Path
from types import SimpleNamespace
import cv2
import numpy as np
ROOT=Path(__file__).resolve().parents[2]
loader=importlib.machinery.SourceFileLoader('calibrate',str(ROOT/'tools/calibration/mine-teleop-calibrate'))
spec=importlib.util.spec_from_loader(loader.name,loader);cal=importlib.util.module_from_spec(spec);loader.exec_module(cal)
spec=importlib.util.spec_from_file_location('quality',ROOT/'tools/calibration/quality.py');quality=importlib.util.module_from_spec(spec);spec.loader.exec_module(quality)
spec=importlib.util.spec_from_file_location('compare',ROOT/'tools/benchmark/compare.py');bench=importlib.util.module_from_spec(spec);spec.loader.exec_module(bench)

def rejects(f):
    try:f()
    except (ValueError,KeyError,TypeError):return
    raise AssertionError('invalid input passed')

def main():
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory);args=SimpleNamespace(out=str(root),length=6,width=2.8,vehicle_id='fixture',survey=None)
        cal.field(args);field=json.loads((root/'field.json').read_text());cal.validate_survey(field)
        for index in range(8):
            board=cv2.imread(str(root/f'A{index+1}.png'));assert cal.detect(board,index) is not None
            assert cal.detect(board,(index+1)%8) is None
        invalid=json.loads(json.dumps(field));invalid['boards']['A1']['u']=[0,2,0];rejects(lambda:cal.validate_survey(invalid))
        invalid=json.loads(json.dumps(field));invalid['check_points'][0]['world']=[0,0,0];rejects(lambda:cal.validate_survey(invalid))
        # Ground inverse agrees with OpenCV's forward model including distortion.
        camera={'K':[300,0,640,0,305,360,0,0,1],'xi':.7,'D':[.01,-.002,.003,-.004],'T_vehicle_from_camera':[1,0,0,0,0,-1,0,0,0,0,-1,2,0,0,0,1],'A_runtime_from_calibration':np.eye(3).flatten().tolist()}
        points=np.array([[-2,-1,0],[0,0,0],[2,.5,0],[1,-2,0]],float)
        pixels=cal.project(points,np.array([math.pi,0,0]),np.array([0.,0,2]),np.asarray(camera['K']).reshape(3,3),camera['xi'],np.asarray(camera['D']))
        for point,pixel in zip(points,pixels):assert np.linalg.norm(cal.ground(camera,pixel)-point)<1e-6
        blank=np.zeros((64,64,4),np.uint8);blank[:,:,3]=255;changed=blank.copy();changed[:20]=255
        mask=np.zeros((64,64),np.uint8);mask[30:]=1
        assert abs(quality.masked_ssim(blank,changed,mask)-1)<1e-8
        changed[45:50,30:35,:3]=255;assert quality.masked_ssim(blank,changed,mask)<1
        # No background score can bypass the concrete independent driving cases.
        assert not quality.passed_quality({'profile':'540p','geometry_hash':'g','cases':[]},'540p','g')
        qualification={'profile':'540p','geometry_hash':'g','ssim_scope':'projection_effective_ground_mask','mask_sha256':'a'*64,'cases':[]}
        for identity,rule in quality.cases().items():
            qualification['cases'].append({'case_id':identity,'size_m':rule['size_m'],'distance_m':rule['distance_m'],'source_contrast':.1,'target_pixels_min':4,'visibility_fraction':.8,'lost_targets':0,'duplicated_targets':0,'blinded_detection_rate':.95,'false_positive_rate':.05,'observers':3,'position_p95_m':.03,'source_clip_sha256':'b'*64,'decoded_clip_sha256':'c'*64})
        assert len(qualification['cases'])==252 and quality.passed_quality(qualification,'540p','g')
        assert not quality.passed_quality(qualification,'720p','g')
        qualification['cases'][0]['lost_targets']=1;assert not quality.passed_quality(qualification,'540p','g')
        qualification['cases'][0]['lost_targets']=0;qualification['cases'][0]['source_contrast']=float('nan');assert not quality.passed_quality(qualification,'540p','g')
        qualification['cases'][0]['source_contrast']=.1;qualification['cases'][0]['source_clip_sha256']='unverified';assert not quality.passed_quality(qualification,'540p','g')
        rejects(lambda:cal.strict_json('{"bad":NaN}'))
        accepted=root/'accepted.json';accepted.write_text('previous accepted calibration')
        draft={'schema':1,'cameras':{},'survey_sha256':'different'};packed=root/'draft.json';cal.atomic(packed,cal.envelope(draft))
        args=SimpleNamespace(calibration=str(packed),field=str(root/'field.json'),checks=str(root/'checks.json'),out=str(accepted))
        (root/'checks.json').write_text('{"observations":[]}')
        rejects(lambda:cal.validate(args));assert accepted.read_text()=='previous accepted calibration'
        # Explicit frame pairing, not a difference between independent P95s.
        rows=[]
        for variant in bench.VARIANTS:
            for run in (1,2,3):
                for second in (30,630):rows.append({'event':'network','variant':variant,'run':run,'at_seconds':second,'vehicle_tx_bytes':second*100,'cloud_media_tx_bytes':second*50,'encoder_video_bytes':second*80,'cpu_percent':10,'rss_bytes':1024})
                for scene in bench.SCENARIOS:rows.append({'variant':variant,'run':run,'scenario':scene,'source_frame_id':'one','path':'drive','optical_latency_ms':100+(20 if variant!='six' else 0),'uncertainty_ms':1})
        result=bench.compare(rows);assert result['variants']['two-bev-540p']['paired_extra_latency']['drive']['p95_ms']==20
        bad=rows+[rows[-1]];rejects(lambda:bench.compare(bad))
    print('PASS numbered boards, OpenCV inverse, independent effective-mask quality, failed acceptance preservation, paired optical measurements (synthetic only)')
if __name__=='__main__':main()
