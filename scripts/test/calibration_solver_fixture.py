#!/usr/bin/env python3
"""Exercise the actual omnidir and pose solvers with exact synthetic corners.

Board detection is checked separately. These generated corner observations do
not qualify a physical lens, exposure clock, installation or driving profile.
"""
import importlib.machinery
import importlib.util
import json
import math
import tempfile
from pathlib import Path
from types import SimpleNamespace
import cv2
import numpy as np

ROOT=Path(__file__).resolve().parents[2]
loader=importlib.machinery.SourceFileLoader('calibrate_solver',str(ROOT/'tools/calibration/mine-teleop-calibrate'))
spec=importlib.util.spec_from_loader(loader.name,loader)
cal=importlib.util.module_from_spec(spec);loader.exec_module(cal)

with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    cal.field(SimpleNamespace(out=str(root),length=6,width=2.8,vehicle_id='synthetic-solver',survey=None))
    field=json.loads((root/'field.json').read_text());field.update(surveyed=True,measurement_accuracy_m=.001,ground_flatness_m=.001)
    cal.atomic(root/'field.json',field)
    K=np.array([[600.,0,640],[0,610,360],[0,0,1]])
    xi=.65;D=np.array([.01,-.002,.0005,-.0004])
    model=cal.board_model(0)
    objects=model.getChessboardCorners().astype(np.float64).reshape(1,-1,3)
    centre=objects.reshape(-1,3).mean(axis=0)
    rng=np.random.default_rng(417)
    detections={};files=[]
    for index in range(40):
        tag=index+1
        rvec=rng.uniform([-.6,-.6,-.4],[.6,.6,.4])
        desired=np.array([(-3 if index%2 else 3),(-2 if index%4<2 else 2),2.7])+rng.uniform(-.25,.25,3)
        tvec=desired-cv2.Rodrigues(rvec)[0]@centre
        pixels=cal.project(objects,rvec,tvec,K,xi,D).reshape(1,-1,2)
        detections[tag]={0:(objects,pixels)}
        image=np.zeros((720,1280,3),np.uint8);image[0,0]=tag
        filename=f'intrinsic-{index}.png';cv2.imwrite(str(root/filename),image);files.append(filename)
    detections[250]={}
    for index in range(8):
        plate=field['boards'][f'A{index+1}']
        world=np.asarray(plate['origin'])+objects.reshape(-1,3)[:,0,None]*plate['u']+objects.reshape(-1,3)[:,1,None]*plate['v']
        pixels=cal.project(world,np.array([math.pi,0,0]),np.array([0.,0,2.]),K,xi,D)
        detections[250][index]=(objects,pixels.reshape(1,-1,2))
    image=np.zeros((720,1280,3),np.uint8);image[0,0]=250;cv2.imwrite(str(root/'ground.png'),image)
    # Feed exact correspondences into the real OpenCV/scipy solve chain.
    cal.detect=lambda image,index:detections[int(image[0,0,0])].get(index)
    dataset={'cameras':{},'samples':[]}
    for camera in cal.IDS:
        dataset['cameras'][camera]={'runtime_size':[1280,720],'device':'fixture:'+camera,'installation_id':'synthetic','capture_width':1280,'capture_height':720,'exposure_time_trusted':False}
        metadata=dataset['cameras'][camera]
        dataset['samples'] += [{'camera_id':camera,'role':'intrinsic','file':name,'metadata':dict(metadata)} for name in files]
        dataset['samples'].append({'camera_id':camera,'role':'extrinsic','file':'ground.png','metadata':dict(metadata)})
    cal.atomic(root/'dataset.json',dataset)
    cal.solve(SimpleNamespace(dataset=str(root),field=str(root/'field.json'),out=str(root/'draft.json')))
    result,_=cal.read_calibration(root/'draft.json')
    for camera in result['cameras'].values():
        assert camera['intrinsic_samples']>=20 and camera['intrinsic_rms_px']<.02
        assert camera['extrinsic_rms_px']<.25
        transform=np.asarray(camera['T_vehicle_from_camera']).reshape(4,4)
        assert np.linalg.norm(transform[:3,3]-[0,0,2])<.01
    assert not result['acceptance']['static_pass'] and not result['acceptance']['dynamic_pass']
    previous=(root/'draft.json').read_bytes()
    dataset['samples'][0]['metadata']['installation_id']='moved-camera'
    cal.atomic(root/'dataset.json',dataset)
    try:cal.solve(SimpleNamespace(dataset=str(root),field=str(root/'field.json'),out=str(root/'draft.json')))
    except ValueError as error:assert 'sample identity' in str(error)
    else:raise AssertionError('mixed camera installation samples accepted')
    dataset['samples'][0]['metadata']['installation_id']='synthetic'
    for sample in dataset['samples']:
        if sample['camera_id']==cal.IDS[0] and sample['role']=='intrinsic':sample['file']=files[0]
    cal.atomic(root/'dataset.json',dataset)
    try:cal.solve(SimpleNamespace(dataset=str(root),field=str(root/'field.json'),out=str(root/'draft.json')))
    except ValueError as error:assert '20 valid intrinsic' in str(error)
    else:raise AssertionError('duplicate photos counted as twenty valid samples')
    assert (root/'draft.json').read_bytes()==previous
    print('PASS actual omnidir intrinsic / ground-pose solve, four camera drafts; synthetic corners only')
