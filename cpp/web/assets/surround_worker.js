'use strict';
importScripts('/assets/surround_core.js');
let modulePromise=createSurroundModule(), mod=null, renderer=0, pointers=[], output=0;
let dimensions=[], width=0,height=0, cal=null;
const ids=['fish_front','fish_rear','fish_left','fish_right'];
let processing=false;
onmessage=async event=>{
  const message=event.data;
  try{
    if(message.type==='init'){
      mod=await modulePromise;cal=message.calibration;width=message.width;height=message.height;
      const parameters=[];
      for(const id of ids){const c=cal.cameras[id];parameters.push(...c.K,c.xi,...c.D,...c.T_vehicle_from_camera,...c.A_runtime_from_calibration,...c.runtime_size);dimensions.push(c.runtime_size);pointers.push(mod._malloc(c.runtime_size[0]*c.runtime_size[1]*4));}
      const params=mod._malloc(parameters.length*8), region=mod._malloc(32);
      mod.HEAPF64.set(parameters,params/8);mod.HEAPF64.set(cal.ground_region,region/8);
      renderer=mod._mt_surround_create(params,region,cal.vehicle_length_m,cal.vehicle_width_m,width,height);
      mod._free(params);mod._free(region);
      if(!renderer)throw Error('标定映射无效');output=mod._malloc(width*height*4);
      postMessage({type:'ready'});return;
    }
    if(message.type!=='frames')return;
    if(processing||!renderer){for(const item of message.frames)item.frame.close();throw Error('环视尚未准备完成');}
    processing=true;const started=performance.now();let mask=0;
    const stamps=message.frames.map(item=>item.aligned),received=message.frames.map(item=>item.received);
    const healthy=message.frames.length===4&&Math.max(...stamps)-Math.min(...stamps)<=(cal.acceptance?.timing?.hard_max_skew_ms||20)&&performance.timeOrigin+started-Math.min(...received)<=100;
    for(const item of message.frames){const index=ids.indexOf(item.id);const frame=item.frame;
      try{if(index<0||frame.displayWidth!==dimensions[index][0]||frame.displayHeight!==dimensions[index][1])throw Error('相机运行尺寸与标定不匹配');
        const rgba=new Uint8Array(frame.allocationSize({format:'RGBA'}));const copyStarted=performance.now();await frame.copyTo(rgba,{format:'RGBA'});
        mod.HEAPU8.set(rgba,pointers[index]);if(healthy)mask|=1<<index;
        postMessage({type:'stage',id:item.id,copy_ms:performance.now()-copyStarted,source_time_quality:'browser_estimate'});
      }finally{frame.close();}
    }
    const composeStarted=performance.now();
    if(mod._mt_surround_render(renderer,...pointers,mask,output)!==0)throw Error('环视合成失败');
    const rgba=mod.HEAPU8.slice(output,output+width*height*4).buffer;
    postMessage({type:'rendered',rgba,healthy,inputs:message.frames.map(({id,rtp_timestamp,received,aligned})=>({id,rtp_timestamp,received,aligned})),compose_ms:performance.now()-composeStarted,worker_ms:performance.now()-started},[rgba]);
  }catch(error){if(message.frames)for(const item of message.frames){try{item.frame.close()}catch{}}postMessage({type:'error',error:error.message});}
  finally{processing=false;}
};
