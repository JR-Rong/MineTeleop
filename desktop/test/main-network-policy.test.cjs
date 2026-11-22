'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const {EventEmitter}=require('node:events');

async function startDesktop(platform){
  const calls=[];
  const app=new EventEmitter();
  Object.assign(app,{isPackaged:false,setName(){},requestSingleInstanceLock:()=>true,
    whenReady:()=>Promise.resolve(),getPath:()=>'/fixture/logs',quit(){}});
  const session=new EventEmitter();
  session.setPermissionRequestHandler=handler=>{session.request=handler;};
  session.setPermissionCheckHandler=handler=>{session.check=handler;};
  let window;
  class BrowserWindow extends EventEmitter{
    constructor(options){
      super();window=this;this.options=options;this.webContents=new EventEmitter();
      this.webContents.setWebRTCIPHandlingPolicy=policy=>calls.push(['policy',policy]);
      this.webContents.setWindowOpenHandler=handler=>{this.open=handler;};
    }
    async loadURL(url){calls.push(['load',url]);}
    maximize(){}
    show(){}
  }
  class NativeController extends EventEmitter{
    async start(){return 'http://127.0.0.1:18888/';}
  }
  const modules={
    electron:{app,BrowserWindow,session:{defaultSession:session},
      Menu:{buildFromTemplate:()=>({}),setApplicationMenu(){}},
      dialog:{showErrorBox:(_title,message)=>{throw Error(message);}},shell:{}},
    'node:path':path,'node:fs':fs,
    './native-controller.cjs':{NativeController},
    './log-directory.cjs':{prepareLogDirectory:()=>'/fixture/logs'},
    './diagnostic-log.cjs':{createDiagnosticLog:()=>({write(){}})},
  };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../main.cjs'),'utf8'),{
    require:name=>{assert.ok(Object.hasOwn(modules,name),name);return modules[name];},
    process:{platform,env:{MINE_TELEOP_DESKTOP_ROOT:'/fixture/native'}},URL,console,
  });
  await new Promise(resolve=>setImmediate(resolve));
  return {calls,session,window};
}

for(const platform of ['win32','linux','darwin']){
test(`${platform}: desktop configures default-interface ICE candidates before loading its local console`,async()=>{
  const {calls}=await startDesktop(platform);
  assert.deepEqual(calls,[['policy','default_public_and_private_interfaces'],['load','http://127.0.0.1:18888/']]);
});

test(`${platform}: ICE policy retains capture denial, renderer isolation and navigation restrictions`,async()=>{
  const {session,window}=await startDesktop(platform);
  const contents={getURL:()=> 'http://127.0.0.1:18888/'};
  for(const permission of ['media','audioCapture','videoCapture','geolocation']){
    let granted;
    session.request(contents,permission,value=>{granted=value;});
    assert.equal(granted,false,permission);
    assert.equal(session.check(contents,permission,'http://127.0.0.1:18888'),false,permission);
  }
  const preferences=window.options.webPreferences;
  assert.equal(preferences.sandbox,true);
  assert.equal(preferences.webSecurity,true);
  assert.equal(preferences.contextIsolation,true);
  assert.equal(preferences.nodeIntegration,false);
  assert.equal(window.open().action,'deny');
  let blocked=false;
  window.webContents.emit('will-navigate',{preventDefault(){blocked=true;}},'https://example.com/');
  assert.equal(blocked,true);
});
}
