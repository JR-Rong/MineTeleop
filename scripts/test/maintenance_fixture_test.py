#!/usr/bin/env python3
"""Actual maintenance capture / legacy configuration, using synthetic camera."""
import fcntl
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import yaml

root=Path(__file__).resolve().parents[2]
runtime=sys.argv[1]
with tempfile.TemporaryDirectory(prefix='mine-maintenance-') as directory:
    folder=Path(directory)
    config=yaml.safe_load((root/'configs/vehicle-agent.dev.yaml').read_text())
    config['recording']={'enabled':True,'root':'/must-not-record'}
    config['upload']={'enabled':True,'endpoint':'https://invalid.test'}
    config['media']['record_profiles']={'old':{'codec':'h264'}}
    config['cameras'][0]['record_profile']='old'
    path=folder/'legacy.yaml';path.write_text(yaml.safe_dump(config))
    sentinel=folder/'existing-video.mp4';sentinel.write_bytes(b'preserve-existing-file')
    checked=subprocess.run([runtime,'config-check','--config',str(path)],capture_output=True,text=True,check=True)
    assert 'deprecated_video_config_ignored' in checked.stderr
    assert json.loads(checked.stdout.splitlines()[-1])['passed']
    retired=subprocess.run([runtime,'vehicle-uploader','--recording-root',str(folder)],capture_output=True,text=True)
    assert retired.returncode==2 and 'removed' in retired.stderr and sentinel.read_bytes()==b'preserve-existing-file'
    field_config=json.loads(json.dumps(config));field_config['field_safety']['commissioning_mode']='field'
    field_path=folder/'field.yaml';field_path.write_text(yaml.safe_dump(field_config))
    diagnostic=subprocess.run([runtime,'vehicle-media-agent','--config',str(field_path),'--diagnostic-partition'],capture_output=True,text=True)
    assert diagnostic.returncode!=0 and 'bench' in diagnostic.stderr
    lock_root=folder/'shared-locks';lock_root.mkdir()
    env={**os.environ,'MINE_TELEOP_CAMERA_LOCK_DIR':str(lock_root)}
    lock=lock_root/('vehicle-'+hashlib.sha256(config['vehicle']['id'].encode()).hexdigest()+'.lock')
    output=folder/'capture.ppm'
    command=[runtime,'calibration-capture','--config',str(path),'--camera-id','front','--output',str(output)]
    with open(lock,'a') as owner:
        fcntl.flock(owner,fcntl.LOCK_EX|fcntl.LOCK_NB)
        denied=subprocess.run(command,env=env,capture_output=True,text=True)
        assert denied.returncode!=0 and not output.exists()
        captured=subprocess.run(command+['--maintenance-lock-fd',str(owner.fileno())],env=env,pass_fds=(owner.fileno(),),capture_output=True,text=True,check=True)
        metadata=json.loads(captured.stdout.splitlines()[-1])
        assert output.read_bytes().startswith(b'P6') and metadata['camera_id']=='front'
        assert not metadata['exposure_time_trusted']
    print('PASS synthetic capture ownership, inherited maintenance lock, truthful timestamp quality and retired video configuration/files')
