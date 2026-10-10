"""Driving qualification rules. Background SSIM is never driving acceptance."""
import math
import re
import cv2
import numpy as np

def cases():
    result={}
    for lighting in ('day','night','dust'):
        for motion in ('static','moving_1mps'):
            for view in ('drive_front','drive_rear'):
                for kind,size,distances in (('person',[.5,1.6],(10,20,30)),('cone',[.3,.5],(5,10,20)),('post',[.1,1.],(5,10,15))):
                    for distance in distances:
                        key=f'{lighting}:{motion}:{view}:{kind}:{distance}'
                        result[key]={'size_m':size,'distance_m':distance,'min_contrast':.1,'position_tolerance_m':None}
            for seam in ('front-left','front-right','rear-left','rear-right'):
                for kind,size in (('obstacle',[.2,.2]),('person',[.5,1.6])):
                    for distance in (.5,1.,2.):
                        key=f'{lighting}:{motion}:{seam}:{kind}:{distance}'
                        result[key]={'size_m':size,'distance_m':distance,'min_contrast':.1,'position_tolerance_m':.1 if motion=='moving_1mps' else .05}
    return result

def passed_quality(document,profile,geometry_hash):
    if document.get('profile')!=profile or document.get('geometry_hash')!=geometry_hash:return False
    expected=cases();observed={row['case_id']:row for row in document.get('cases',[])}
    if set(observed)!=set(expected) or len(document['cases'])!=len(expected):return False
    for key,rule in expected.items():
        row=observed[key]
        if row.get('size_m')!=rule['size_m'] or row.get('distance_m')!=rule['distance_m']:return False
        if not math.isfinite(row.get('source_contrast',float('nan'))) or row['source_contrast']<.1:return False
        for key_name in ('target_pixels_min','visibility_fraction','blinded_detection_rate','false_positive_rate','observers'):
            value=row.get(key_name)
            if not isinstance(value,(int,float)) or not math.isfinite(value) or value<0:return False
        if row['visibility_fraction']>1 or row['blinded_detection_rate']>1 or row['false_positive_rate']>1:return False
        if row.get('target_pixels_min',0)<4 or row.get('visibility_fraction',0)<.8:return False
        if row.get('lost_targets',1) or row.get('duplicated_targets',1):return False
        if row.get('blinded_detection_rate',0)<.95 or row.get('false_positive_rate',1)>.05 or row.get('observers',0)<3:return False
        error=row.get('position_p95_m')
        if rule['position_tolerance_m'] is not None and (error is None or not math.isfinite(error) or error>rule['position_tolerance_m']):return False
        if any(not isinstance(row.get(name),str) or not re.fullmatch('[0-9a-f]{64}',row[name]) for name in ('source_clip_sha256','decoded_clip_sha256')):return False
    # The benchmark must derive this mask from the shared projection mapping,
    # excluding the body and uncovered pixels, before averaging local SSIM.
    return document.get('ssim_scope')=='projection_effective_ground_mask' and isinstance(document.get('mask_sha256'),str) and bool(re.fullmatch('[0-9a-f]{64}',document['mask_sha256']))

def masked_ssim(reference,decoded,mask):
    if reference.shape!=decoded.shape or mask.shape!=reference.shape[:2]:raise ValueError('SSIM shape mismatch')
    # Exclude filter windows touching the vehicle or blank coverage boundaries.
    valid=cv2.erode((mask>0).astype(np.uint8),np.ones((11,11),np.uint8))>0
    if not valid.any():raise ValueError('no valid ground SSIM windows')
    x=reference[:,:,:3].astype(np.float64);y=decoded[:,:,:3].astype(np.float64)
    ux=cv2.GaussianBlur(x,(11,11),1.5);uy=cv2.GaussianBlur(y,(11,11),1.5)
    vx=cv2.GaussianBlur(x*x,(11,11),1.5)-ux*ux;vy=cv2.GaussianBlur(y*y,(11,11),1.5)-uy*uy
    xy=cv2.GaussianBlur(x*y,(11,11),1.5)-ux*uy
    score=((2*ux*uy+6.5025)*(2*xy+58.5225))/((ux*ux+uy*uy+6.5025)*(vx+vy+58.5225))
    return float(score[valid].mean())
