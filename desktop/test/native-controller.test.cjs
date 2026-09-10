const test=require('node:test');
const assert=require('node:assert/strict');
const {EventEmitter}=require('node:events');
const {PassThrough}=require('node:stream');
const {NativeController,controllerEnvironment}=require('../native-controller.cjs');
function fake(){const child=new EventEmitter();child.stdin=new PassThrough();child.stdout=new PassThrough();child.stderr=new PassThrough();child.kill=signal=>{child.emit('exit',null,signal);return true;};return child;}
function started(child,port=43210){child.stdout.write(JSON.stringify({event:'control_client_started',host:'127.0.0.1',port,url:`http://127.0.0.1:${port}/`})+'\n');}
test('launch uses explicit bundled paths and private pipe, never a shell or default browser',async()=>{
  const child=fake();let launch;
  const controller=new NativeController({root:'/bundle with spaces',logPath:'/user/log',spawnProcess:(...args)=>{launch=args;return child;}});
  const ready=controller.start();started(child);assert.equal(await ready,'http://127.0.0.1:43210/');
  assert.equal(launch[2].shell,false);assert.equal(launch[2].windowsHide,true);assert(launch[1].includes('--desktop-managed'));assert(launch[1].includes('--no-open-browser'));
  let command='';child.stdin.on('data',data=>command+=data);child.stdin.on('finish',()=>child.emit('exit',0,null));
  assert.equal(controller.stop(),controller.stop());assert.deepEqual(await controller.stop(),{forced:false});assert.equal(command,'shutdown\n');
});
test('startup refuses a non-loopback or mismatched ready address',async()=>{
  const child=fake();child.stdin.on('finish',()=>child.emit('exit',0,null));
  const controller=new NativeController({root:'/bundle',logPath:'/log',spawnProcess:()=>child});const ready=controller.start();
  child.stdout.write(JSON.stringify({event:'control_client_started',host:'0.0.0.0',port:80,url:'http://example.com/'})+'\n');
  await assert.rejects(ready,/无效/);await controller.stop();
});
test('a stuck child is killed only after the bounded graceful shutdown interval',async()=>{
  const child=fake();const controller=new NativeController({root:'/bundle',logPath:'/log',shutdownMs:15,spawnProcess:()=>child});
  const ready=controller.start();started(child);await ready;assert.deepEqual(await controller.stop(),{forced:true});
});
test('unexpected native exit is observable and a later stop resolves',async()=>{
  const child=fake();const controller=new NativeController({root:'/bundle',logPath:'/log',spawnProcess:()=>child});let exit;
  controller.on('exit',event=>exit=event);const ready=controller.start();started(child);await ready;child.emit('exit',1,null);
  assert.equal(exit.expected,false);assert.deepEqual(await controller.stop(),{forced:false});
});
test('native launch ignores ambient controller overrides and loader injection',()=>{
  const env=controllerEnvironment('/bundle',{PATH:'/usr/bin',MINE_TELEOP_DRIVER_PASSWORD:'secret',MINE_TELEOP_CONFIG:'/wrong',LD_PRELOAD:'/injected',NODE_OPTIONS:'--inspect'},'linux');
  assert.equal(env.MINE_TELEOP_DRIVER_PASSWORD,undefined);assert.equal(env.MINE_TELEOP_CONFIG,undefined);assert.equal(env.LD_PRELOAD,undefined);assert.equal(env.NODE_OPTIONS,undefined);assert.equal(env.LD_LIBRARY_PATH,'/bundle/lib');
});
test('spawn failure does not leave shutdown waiting for an exit event that never arrives',async()=>{
  const child=fake();const controller=new NativeController({root:'/missing',logPath:'/log',spawnProcess:()=>child});
  const ready=controller.start();child.emit('error',new Error('ENOENT'));await assert.rejects(ready,/ENOENT/);assert.deepEqual(await controller.stop(),{forced:false});
});
