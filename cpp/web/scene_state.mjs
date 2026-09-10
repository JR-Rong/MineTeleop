export class ViewState {
  constructor({stopSpeed=0.05,moveSpeed=0.08,settleMs=300,resumeMs=5000,staleMs=500}={}) {
    Object.assign(this,{stopSpeed,moveSpeed,settleMs,resumeMs,staleMs});
    this.mode='top';this.candidate=null;this.manual=false;this.resumeAt=0;this.sample=null;
  }
  receive(telemetry,now,wallNow=Date.now()) {
    const feedback=telemetry && telemetry.can_feedback;
    const speed=feedback && feedback.supported ? (feedback.speed_valid&&feedback.feedback_fresh?feedback.speed_mps:null) : (telemetry && telemetry.speed_mps);
    const timestamp=telemetry && telemetry.sent_at_utc_ms;
    const valid=typeof speed==='number'&&Number.isFinite(speed)&&typeof timestamp==='number'&&Number.isFinite(timestamp);
    this.sample=valid?{speed:Math.abs(speed),at:now,age:Math.max(0,wallNow-timestamp)}:null;
    if(!valid)this.candidate=null;
  }
  beginManual(){this.manual=true;this.candidate=null;}
  endManual(now){this.manual=false;this.resumeAt=now+this.resumeMs;}
  resume(){this.manual=false;this.resumeAt=0;}
  step(now){
    const fresh=Boolean(this.sample&&this.sample.age+now-this.sample.at<=this.staleMs);
    const manual=this.manual||now<this.resumeAt;
    if(!fresh){this.candidate=null;return {mode:this.mode,fresh:false,manual};}
    if(manual){this.candidate=null;return {mode:this.mode,fresh:true,manual};}
    const speed=this.sample.speed;
    const wanted=speed<=this.stopSpeed?'top':speed>=this.moveSpeed?'rear':this.mode;
    if(wanted===this.mode)this.candidate=null;
    else if(!this.candidate || this.candidate.mode!==wanted)this.candidate={mode:wanted,since:now};
    else if(now-this.candidate.since>=this.settleMs){this.mode=wanted;this.candidate=null;}
    return {mode:this.mode,fresh:true,manual:false};
  }
}

export function validateManifest(value){
  if(!value||typeof value.file!=='string'||!value.file||!/\.(glb|gltf|fbx|obj)$/i.test(value.file))throw Error('模型格式应为 GLB、glTF、FBX 或 OBJ');
  const scale=value.scale == null ? 1 : value.scale;
  if(!Number.isFinite(scale)||scale<=0||scale>10000)throw Error('模型缩放必须是有效正数');
  for(const key of ['rotation_deg','offset_m'])if(value[key]!=null&&(!Array.isArray(value[key])||value[key].length!==3||!value[key].every(Number.isFinite)))throw Error(`${key} 必须包含三个有限数值`);
  if(value.material!=null&&(typeof value.material!=='string'||!value.material.toLowerCase().endsWith('.mtl')))throw Error('OBJ 材质必须是 MTL 文件');
  const camera={stopSpeed:0.05,moveSpeed:0.08,settleMs:300,resumeMs:5000,staleMs:500,...value.camera};
  if(!Object.values(camera).every(v=>typeof v==='number'&&Number.isFinite(v)&&v>=0)||camera.moveSpeed<=camera.stopSpeed||camera.staleMs>2000||camera.settleMs>5000||camera.resumeMs>60000)throw Error('自动视角参数无效');
  return {...value,scale,rotation_deg:value.rotation_deg == null ? [0,0,0] : value.rotation_deg,offset_m:value.offset_m == null ? [0,0,0] : value.offset_m,camera};
}
