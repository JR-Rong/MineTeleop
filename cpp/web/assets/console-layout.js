(() => {
  const main=document.querySelector('.app-shell'),workspace=document.querySelector('.workspace');
  if(!main||!workspace)return;
  const overview=document.createElement('section');overview.className='vehicle-overview';overview.setAttribute('aria-label','车辆与八轮 CAN 实时反馈');
  workspace.before(overview);
  overview.append(document.getElementById('operator-status-strip'),document.getElementById('can-feedback-panel'));
  const sidebar=workspace.querySelector('.sidebar');
  const vehicle=document.createElement('section');vehicle.className='side-section vehicle-state';
  vehicle.innerHTML='<div class="section-heading"><h2>车辆状态</h2></div>';
  vehicle.append(document.querySelector('.can-summary'));
  sidebar.querySelector('.side-section').after(vehicle);
  for(const [id,label] of [['vcu-connect','开始握手'],['vcu-disconnect','断开握手']]){
    const button=document.getElementById(id);button.title=button.textContent;button.textContent=label;
  }
  const cameraHeader=document.createElement('div');cameraHeader.className='camera-toolbar';
  cameraHeader.innerHTML='<strong>相机画面</strong><span class="camera-layout-active">多路平铺</span><button type="button" disabled title="环视拼接与相机绑定将在后续接入">360° 环视 · 待接入</button>';
  document.querySelector('.visual-stage').prepend(cameraHeader);
  const scene=document.createElement('section');scene.className='scene-panel';scene.setAttribute('aria-label','自车三维场景');
  scene.innerHTML='<div class="scene-heading"><h2>3D 场景</h2><button class="scene-resume" type="button" disabled>恢复自动</button></div><div class="scene-view-status">等待模型</div><div class="scene-canvas"></div><div class="scene-status" role="status">正在初始化 3D…</div>';
  sidebar.before(scene);
  scene.querySelector('.scene-resume').addEventListener('click',event=>event.currentTarget.blur());
  const videos=document.getElementById('cameras');
  const resizeGrid=()=>{
    const count=videos.querySelectorAll('.camera').length;
    const columns=count===1||(count===2&&videos.clientWidth/videos.clientHeight<16/9)?1:2;
    videos.dataset.cameraCount=String(count);
    videos.style.setProperty('--camera-columns',String(columns));
    videos.style.setProperty('--camera-rows',String(count===3?2:Math.max(1,Math.ceil(count/columns))));
  };
  new MutationObserver(resizeGrid).observe(videos,{childList:true});
  new ResizeObserver(resizeGrid).observe(videos);resizeGrid();
  fetch('/api/scene-config').then(response=>{if(!response.ok)throw Error('无法读取 3D 配置');return response.json();}).then(async config=>{
    if(!config.enabled){scene.querySelector('.scene-status').textContent='3D 已在配置中关闭';return;}
    const {mountScene}=await import('./scene.js');await mountScene(scene,config.manifest);
  }).catch(error=>{scene.querySelector('.scene-status').textContent=`3D 不可用：${error.message}`;});
})();
