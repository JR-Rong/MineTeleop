import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
import {ViewState,validateManifest} from './scene_state.mjs';

export async function mountScene(host,manifestURL){
  const canvasHost=host.querySelector('.scene-canvas');
  const status=host.querySelector('.scene-status');
  const viewLabel=host.querySelector('.scene-view-status');
  const setStatus=text=>{if(status.textContent!==text)status.textContent=text;};
  let renderer,controls,observer,frame=0,disposed=false,model;
  const scene=new THREE.Scene();
  scene.background=new THREE.Color('#0b1523');
  const stop=()=>{disposed=true;cancelAnimationFrame(frame);observer?.disconnect();controls?.dispose();scene.traverse(obj=>{obj.geometry?.dispose();for(const m of (Array.isArray(obj.material)?obj.material:[obj.material]).filter(Boolean)){for(const v of Object.values(m))if(v?.isTexture)v.dispose();m.dispose();}});renderer?.dispose();};
  addEventListener('pagehide',stop,{once:true});
  try{
    const response=await fetch(manifestURL);if(!response.ok)throw Error('找不到模型配置');
    const manifest=validateManifest(await response.json());
    const base=new URL(manifestURL,location.href);
    const manager=new THREE.LoadingManager();
    manager.setURLModifier(url=>{
      if(url.startsWith('blob:')||url.startsWith('data:'))return url;
      const resolved=new URL(url,base);
      if(resolved.origin!==location.origin||!resolved.pathname.startsWith('/assets/'))throw Error('模型资源必须位于本地 assets 目录');
      return resolved.href;
    });
    let resourceFailed=false;
    manager.onError=()=>{resourceFailed=true;setStatus('部分模型资源加载失败，请检查材质和贴图');};
    renderer=new THREE.WebGLRenderer({antialias:true});
    renderer.setPixelRatio(Math.min(devicePixelRatio,1.5));
    renderer.outputColorSpace=THREE.SRGBColorSpace;
    canvasHost.append(renderer.domElement);
    renderer.domElement.setAttribute('aria-label','自车三维视图，可鼠标拖动旋转和滚轮缩放');
    const camera=new THREE.PerspectiveCamera(42,1,0.05,1000);
    controls=new OrbitControls(camera,renderer.domElement);
    controls.enablePan=false;controls.enableDamping=true;controls.dampingFactor=0.12;
    controls.maxPolarAngle=Math.PI/2-0.02;controls.minPolarAngle=0;
    // No keyboard listener: Arrow/WASD remain exclusively vehicle controls.
    const state=new ViewState(manifest.camera);
    let lastTelemetry,transition=null,previousMode='top',wasManual=false,lastDraw=0;
    const receive=event=>{const value=event.detail;if(value!==lastTelemetry){lastTelemetry=value;state.receive(value,performance.now());}};
    document.addEventListener('mine-teleop-telemetry',receive);
    addEventListener('pagehide',()=>document.removeEventListener('mine-teleop-telemetry',receive),{once:true});
    controls.addEventListener('start',()=>state.beginManual());
    controls.addEventListener('end',()=>state.endManual(performance.now()));
    host.querySelector('.scene-resume').onclick=()=>{state.resume();transition=null;wasManual=true;};
    scene.add(new THREE.HemisphereLight(0xdbeafe,0x536079,2.3));
    const light=new THREE.DirectionalLight(0xffffff,2.4);light.position.set(5,12,7);scene.add(light);
    setStatus('正在加载自车模型…');
    const url=new URL(manifest.file,base).href;
    const extension=manifest.file.split('.').pop().toLowerCase();
    if(extension==='fbx'){
      const {FBXLoader}=await import('three/addons/loaders/FBXLoader.js');
      model=await new FBXLoader(manager).loadAsync(url);
    }else if(extension==='obj'){
      const {OBJLoader}=await import('three/addons/loaders/OBJLoader.js');
      const loader=new OBJLoader(manager);
      if(manifest.material){const {MTLLoader}=await import('three/addons/loaders/MTLLoader.js');const material=await new MTLLoader(manager).loadAsync(new URL(manifest.material,base).href);material.preload();loader.setMaterials(material);}
      model=await loader.loadAsync(url);
    }else model=(await new GLTFLoader(manager).loadAsync(url)).scene;
    if(disposed){scene.add(model);stop();return;}
    model.scale.setScalar(manifest.scale);
    model.rotation.set(...manifest.rotation_deg.map(THREE.MathUtils.degToRad));
    model.position.set(...manifest.offset_m);
    scene.add(model);model.updateMatrixWorld(true);
    const box=new THREE.Box3().setFromObject(model),size=box.getSize(new THREE.Vector3());
    if(!Number.isFinite(size.length())||size.length()<=0)throw Error('模型没有可显示的几何体');
    const target=box.getCenter(new THREE.Vector3());
    const grid=new THREE.GridHelper(Math.max(1,size.length()*2),20,0x365371,0x1c2b3c);grid.position.y=box.min.y-size.length()*0.002;scene.add(grid);
    let distance=Math.max(size.length()*1.8,0.1);
    const pose=mode=>target.clone().add(mode==='top'?new THREE.Vector3(0,distance,-distance*0.000001):new THREE.Vector3(0,distance*0.62,-distance*0.85));
    camera.position.copy(pose('top'));controls.target.copy(target);controls.update();
    host.querySelector('.scene-resume').disabled=false;
    observer=new ResizeObserver(()=>{
      const {width,height}=canvasHost.getBoundingClientRect();if(width<=0||height<=0)return;
      renderer.setSize(width,height,false);camera.aspect=width/height;
      // Fit the bounding sphere with margin, including very narrow columns.
      const halfFov=Math.min(THREE.MathUtils.degToRad(camera.fov/2),Math.atan(Math.tan(THREE.MathUtils.degToRad(camera.fov/2))*camera.aspect));
      const fitted=Math.max(size.length()*0.56/Math.sin(halfFov),0.1);
      const ratio=fitted/distance;
      camera.position.sub(target).multiplyScalar(ratio).add(target);
      if(transition){
        transition.from.sub(target).multiplyScalar(ratio).add(target);
        transition.to.sub(target).multiplyScalar(ratio).add(target);
      }
      distance=fitted;
      controls.minDistance=distance*0.45;controls.maxDistance=distance*3;
      camera.near=Math.max(size.length()/10000,0.0001);camera.far=Math.max(1000,distance*10);
      camera.updateProjectionMatrix();controls.update();
    });observer.observe(canvasHost);
    setStatus(`${manifest.name||'自车模型'} · ${resourceFailed?'部分材质或贴图缺失':'感知数据未接入'}`);
    const render=now=>{
      if(disposed)return;
      frame=requestAnimationFrame(render);
      if(document.hidden||now-lastDraw<1000/30)return;lastDraw=now;
      const result=state.step(now);
      const label=result.manual?'手动视角':!result.fresh?'车速无效 · 保持视角':result.mode==='top'?'自动 · 垂直俯视':'自动 · 车后斜视';
      if(viewLabel.textContent!==label)viewLabel.textContent=label;
      if(result.manual||!result.fresh){transition=null;controls.update();}
      else{
        if(result.mode!==previousMode||wasManual){transition={at:now,from:camera.position.clone(),to:pose(result.mode)};}
        if(transition){const t=Math.min(1,(now-transition.at)/500),smooth=t*t*(3-2*t);camera.position.lerpVectors(transition.from,transition.to,smooth);controls.target.copy(target);controls.update();if(t===1)transition=null;}
        else controls.update();
      }
      previousMode=result.mode;wasManual=result.manual||!result.fresh;
      renderer.render(scene,camera);
    };
    frame=requestAnimationFrame(render);
    renderer.domElement.addEventListener('webglcontextlost',event=>{event.preventDefault();setStatus('3D 图形上下文已丢失，刷新页面后重试');cancelAnimationFrame(frame);});
  }catch(error){stop();setStatus(`3D 不可用：${error.message}`);}
}
