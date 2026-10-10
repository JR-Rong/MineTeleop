// Browser plugin was unavailable. Playwright renders the native C++ console;
// only camera tracks/calibration are synthetic, and no driving command is sent.
import http from 'node:http';
import {spawn} from 'node:child_process';
import {createRequire} from 'node:module';
import {mkdir} from 'node:fs/promises';
import assert from 'node:assert/strict';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const context=process.env.TEST_DOCKER_CONTEXT||'colima-mine-teleop-test',container=process.env.TEST_CONTAINER||'mine-teleop-feature-check',nativePort=process.env.TEST_CONSOLE_PORT||'18678';
const server=http.createServer((req,res)=>{
  const child=spawn('docker',['--context',context,'exec','-i',container,'curl','-sS','-D','-','-X',req.method,'-H','Content-Type: application/json','--data-binary','@-',`http://127.0.0.1:${nativePort}${req.url}`]);
  const chunks=[];child.stdout.on('data',chunk=>chunks.push(chunk));req.pipe(child.stdin);
  child.on('close',code=>{if(code){res.writeHead(502);res.end('native proxy failed');return;}const data=Buffer.concat(chunks),end=data.indexOf('\r\n\r\n'),headers=data.subarray(0,end).toString(),status=Number(headers.split(' ')[1]);const type=headers.match(/content-type:\s*([^\r\n]+)/i)?.[1]||'text/plain';res.writeHead(status,{'content-type':type});res.end(data.subarray(end+4));});
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const browser=await chromium.launch({headless:true,channel:process.env.TEST_BROWSER_CHANNEL||undefined,args:['--enable-unsafe-swiftshader']});
const page=await browser.newPage({viewport:{width:1600,height:1000}}),errors=[];
page.on('pageerror',e=>errors.push(e.message));
try{
  await page.goto(`http://127.0.0.1:${server.address().port}/`,{waitUntil:'domcontentloaded'});await page.waitForSelector('#cameras');
  assert.match(await page.title(),/Mine.*Teleop/i);assert.equal(await page.locator('main').count(),1);
  await page.evaluate(()=>{
    const fixtureStream=(id,w=1280,h=720)=>{const canvas=document.createElement('canvas');canvas.width=w;canvas.height=h;const ctx=canvas.getContext('2d');let n=0;const draw=()=>{ctx.fillStyle=id.includes('rear')?'#38526b':'#335f63';ctx.fillRect(0,0,w,h);ctx.fillStyle='#91a8a5';ctx.fillRect(0,h*.55,w,h*.45);ctx.strokeStyle='#e9d7ad';ctx.lineWidth=8;ctx.beginPath();ctx.moveTo(w*.2,h);ctx.lineTo(w*.46,h*.55);ctx.moveTo(w*.8,h);ctx.lineTo(w*.54,h*.55);ctx.stroke();ctx.fillStyle='#f2eee3';ctx.font='36px sans-serif';ctx.fillText(id+' '+n++,40,60);};draw();setInterval(draw,34);return canvas.captureStream(30).getVideoTracks()[0];};
    globalThis.fixtureStream=fixtureStream;
    localStorage.setItem('mine-teleop-layout:BROWSER-FIXTURE:two','null');
    const streams=[{stream_id:'drive_mosaic',camera_id:'drive_mosaic',kind:'drive_mosaic',width:1280,height:1440,regions:[{camera_id:'drive_front',x:0,y:0,width:1280,height:720},{camera_id:'drive_rear',x:0,y:720,width:1280,height:720}]},{stream_id:'surround_bev',camera_id:'surround_bev',kind:'surround_bev',width:512,height:896}];
    cameraView.configure(streams.map(s=>s.stream_id),{vehicleId:'BROWSER-FIXTURE',mode:'two',streams});
    globalThis.fixtureTracks={drive:fixtureStream('drive_mosaic',1280,1440),bev:fixtureStream('surround_bev',512,896)};
    cameraView.attach('drive_mosaic',fixtureTracks.drive);cameraView.attach('surround_bev',fixtureTracks.bev);
  });
  await page.waitForSelector('#camera-drive_front canvas');assert.equal(await page.locator('#cameras .camera').count(),3);assert.equal(await page.locator('.camera-resize').count(),12);
  await page.waitForFunction(()=>cameraView.healthyStreams().length===2,{},{timeout:10000});
  await page.evaluate(()=>fixtureTracks.bev.stop());
  await page.waitForFunction(()=>!cameraView.healthyStreams().includes('surround_bev'),{},{timeout:1000});
  const first=page.locator('#camera-drive_front');const original=await first.evaluate(e=>e.style.gridColumn);
  await first.dblclick({position:{x:70,y:60}});assert.equal(await first.evaluate(e=>e.style.gridColumn),'span 10');await first.dblclick({position:{x:70,y:60}});assert.equal(await first.evaluate(e=>e.style.gridColumn),original);
  for(const corner of ['nw','ne','sw','se']){
    const handle=first.locator('.camera-resize.'+corner);await handle.scrollIntoViewIfNeeded();const box=await handle.boundingBox();await page.mouse.move(box.x+8,box.y+8);await page.mouse.down();await page.mouse.move(box.x+8+(corner.endsWith('w')?-80:80),box.y+8,{steps:5});await page.mouse.up();
    const shape=await first.boundingBox();assert.ok(Math.abs(shape.width/shape.height-16/9)<.06,'resize breaks camera aspect');
  }
  const manual=await first.evaluate(e=>e.style.gridColumn);await first.dblclick({position:{x:70,y:60}});await first.dblclick({position:{x:70,y:60}});assert.equal(await first.evaluate(e=>e.style.gridColumn),manual);
  await page.evaluate(()=>cameraView.configure(['drive_mosaic','surround_bev'],{vehicleId:'BROWSER-FIXTURE',mode:'two',streams:[{stream_id:'drive_mosaic',kind:'drive_mosaic',regions:[{camera_id:'drive_front',x:0,y:0,width:1280,height:720},{camera_id:'drive_rear',x:0,y:720,width:1280,height:720}]},{stream_id:'surround_bev',kind:'surround_bev',width:512,height:896}]}));
  assert.equal(await first.evaluate(e=>e.style.gridColumn),manual);await page.reload();
  await page.evaluate(()=>{
    const ids=['drive_front','drive_rear','fish_front','fish_rear','fish_left','fish_right'];
    const K=[40,0,50,0,40,50,0,0,1],T=[1,0,0,0,0,-1,0,0,0,0,-1,2,0,0,0,1],A=[1,0,0,0,1,0,0,0,1];
    const calibration={cameras:Object.fromEntries(ids.filter(i=>i.startsWith('fish_')).map(id=>[id,{K,xi:1,D:[0,0,0,0],T_vehicle_from_camera:T,A_runtime_from_calibration:A,runtime_size:[100,100]}])),ground_region:[-6,6,-4.4,4.4],vehicle_length_m:6,vehicle_width_m:2.8,acceptance:{profiles:{full:true},timing:{hard_max_skew_ms:40}}};
    cameraView.configure(ids,{vehicleId:'WASM-FIXTURE',mode:'full',streams:ids.map(id=>({stream_id:id,camera_id:id,width:100,height:100})),calibration});
    globalThis.fixtureRendered=0;document.querySelector('#cameras').addEventListener('media-frame-presented',e=>{if(e.detail.association==='worker_source_rtp')fixtureRendered++;});
    for(const id of ids){const c=document.createElement('canvas');c.width=100;c.height=100;const x=c.getContext('2d');setInterval(()=>{x.fillStyle='#738f84';x.fillRect(0,0,100,100);x.fillStyle='#e1b955';x.fillRect(Math.random()*60,20,20,20);},33);cameraView.attach(id,c.captureStream(30).getVideoTracks()[0]);}
  });
  await page.waitForFunction(()=>fixtureRendered>=3,{},{timeout:15000});assert.equal(await page.locator('#camera-surround_bev canvas').count(),1);
  const ratio=await page.locator('#camera-surround_bev').evaluate(e=>e.style.aspectRatio.split('/').map(Number));
  assert.ok(Math.abs(ratio[0]/ratio[1]-512/896)<1e-6);
  await page.locator('#surround-toggle').click();await page.locator('#camera-surround_bev').scrollIntoViewIfNeeded();
  await mkdir(process.env.TEST_SCREENSHOTS||'/tmp/mine-teleop-browser-qa',{recursive:true});await page.screenshot({path:(process.env.TEST_SCREENSHOTS||'/tmp/mine-teleop-browser-qa')+'/surround.png',fullPage:true});
  assert.deepEqual(errors,[]);console.log(JSON.stringify({result:'passed',page:'native C++ console',tracks:'synthetic',corners:4,double_click_restore:true,layout_reconnect:true,worker:'real CPU WASM / transferred VideoFrame',screenshots:process.env.TEST_SCREENSHOTS||'/tmp/mine-teleop-browser-qa',page_errors:errors}));
}catch(error){await mkdir(process.env.TEST_SCREENSHOTS||'/tmp/mine-teleop-browser-qa',{recursive:true});await page.screenshot({path:(process.env.TEST_SCREENSHOTS||'/tmp/mine-teleop-browser-qa')+'/failure.png'});console.error(JSON.stringify({page_errors:errors}));throw error;}finally{await browser.close();await new Promise(resolve=>server.close(resolve));}
