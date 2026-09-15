import http from 'node:http';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
// Local-only UI fixture. Every non-asset API request is intercepted; no vehicle session is created.
const upstream=process.argv[2]||'http://127.0.0.1:4388';
const port=Number(process.argv[3]||4389);
const sourceFBX=process.argv[4];
const fixtureRoot=fileURLToPath(new URL('../../cpp/tests/fixtures/console-scene/',import.meta.url));
const original=await (await fetch(upstream+'/')).text();
const guard=`<script>const nativeFetch=window.fetch.bind(window);window.fetch=(url,options)=>{const path=new URL(url instanceof Request?url.url:String(url),location.href).pathname;return String(url instanceof Request?url.url:url).startsWith('blob:')||path.startsWith('/assets/')||path==='/api/scene-config'?nativeFetch(path==='/api/scene-config'?'/api/scene-config?model='+new URLSearchParams(location.search).get('model'):url,options):Promise.resolve(new Response('{}',{headers:{'Content-Type':'application/json'}}));};</script>`;
const fixture=`<script src="/fixture.js"></script>`;
const code=`
addEventListener('DOMContentLoaded',()=>{
  authenticated=true;loginPanel.hidden=true;sessionPanel.hidden=false;vcuPanel.hidden=false;monitorPanel.hidden=false;
  let speed=0,send=true,seq=0;
  const bar=document.createElement('div');bar.className='auth';bar.innerHTML='<strong style="color:#fbbf24">本地模拟 · 无车辆连接</strong>'+['停车','行驶','倒车','停止遥测'].map(x=>'<button type="button">'+x+'</button>').join('');document.querySelector('.topbar').append(bar);
  bar.addEventListener('click',e=>{if(e.target.tagName!=='BUTTON')return;send=e.target.textContent!=='停止遥测';speed=e.target.textContent==='行驶'?2:e.target.textContent==='倒车'?-1:0;});
  cameraGrid.replaceChildren();
  for(let i=0;i<6;i++){const cell=document.createElement('div');cell.className='camera';cell.innerHTML='<div class="label">模拟相机 '+(i+1)+'</div><canvas width="640" height="360" style="width:100%;height:100%;object-fit:contain"></canvas>';cameraGrid.append(cell);const c=cell.lastChild.getContext('2d');let n=0;setInterval(()=>{n++;c.fillStyle='#192737';c.fillRect(0,0,640,180);c.fillStyle='#575246';c.fillRect(0,180,640,180);c.strokeStyle='#d4c18c';c.beginPath();c.moveTo(260,180);c.lineTo(90,360);c.moveTo(380,180);c.lineTo(550,360);c.stroke();c.fillStyle='#bbc9d8';c.font='24px monospace';c.fillText('SIM '+(i+1)+' / '+n,240,110);},100);}
  const a=(n,v)=>Array(n).fill(v);
  setInterval(()=>{
    if(send)vehicleTelemetry={seq:++seq,speed_mps:speed,gear:speed<0?'R':speed>0?'D':'N',sent_at_utc_ms:Date.now(),can_feedback:{supported:true,feedback_fresh:true,max_feedback_age_ms:12,speed_valid:true,speed_mps:speed,gear_valid:true,gear:speed<0?2:speed>0?3:1,driver_gear_request_valid:true,driver_gear_request:1,parking_brake_valid:a(4,true),parking_brake_status:a(4,2),handshake_valid:true,handshake_status:3,vmc_fault_code_valid:true,vmc_fault_code:0,parking_brake_switch_valid:true,parking_brake_switch:1,brake_pedal_switch_valid:true,brake_pedal_switch:0,emergency_switch:0,motor_torque_nm:a(8,125),motor_torque_valid:a(8,true),motor_speed_rpm:a(8,speed*80),motor_speed_valid:a(8,true),brake_pressure_bar:a(8,3.5),brake_valid:a(8,true),motor_mode_valid:a(8,true),motor_mode:a(8,1),brake_mode:a(8,1),steering_valid:a(4,true),steering_angle_deg:a(4,0),steering_mode:a(4,1)}};
    renderMonitoring();document.querySelectorAll('button').forEach(b=>{if(!bar.contains(b)&&!b.classList.contains('scene-resume'))b.disabled=true;});statusPanel.textContent='模拟数据验收页面：相机为合成画面，CAN 为测试数据，不连接真实车辆。';
  },100);
});`;
http.createServer(async(req,res)=>{
  if(req.method!=='GET'){res.writeHead(405).end();return;}
  if(req.url==='/'||req.url.startsWith('/?model=')){res.setHeader('Content-Type','text/html; charset=utf-8');res.end(original.replace('<head>','<head>'+guard).replace('</body>',fixture+'</body>'));return;}
  if(req.url==='/fixture.js'){res.setHeader('Content-Type','text/javascript');res.end(code);return;}
  if(req.url.startsWith('/api/scene-config?model=')&&!req.url.endsWith('null')){const kind=req.url.endsWith('fbx')?'fbx':'obj';res.setHeader('Content-Type','application/json');res.end(JSON.stringify({enabled:true,manifest:'/assets/qa-models/model-'+kind+'.json'}));return;}
  if(req.url.startsWith('/assets/qa-models/')){const file=req.url.split('/').pop();if(!['model-obj.json','model-fbx.json','truck.obj','truck.mtl','carcz.fbx'].includes(file)){res.writeHead(404).end();return;}try{res.end(await readFile(file==='carcz.fbx'?sourceFBX:fixtureRoot+file));}catch{res.writeHead(404).end('Test resource unavailable');}return;}
  if(req.url.startsWith('/assets/')||req.url.startsWith('/api/scene-config')){const r=await fetch(upstream+req.url);res.writeHead(r.status,{'Content-Type':r.headers.get('content-type')});res.end(Buffer.from(await r.arrayBuffer()));return;}
  res.setHeader('Content-Type','application/json');res.end('{}');
}).listen(port,'127.0.0.1',()=>console.log('QA http://127.0.0.1:'+port+'/'));
