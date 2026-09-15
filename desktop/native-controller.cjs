'use strict';
const {spawn}=require('node:child_process');
const {EventEmitter}=require('node:events');
const path=require('node:path');
const fs=require('node:fs');

function controllerEnvironment(root,source=process.env,platform=process.platform){
  const env={...source};
  for(const key of Object.keys(env)){
    if(key.startsWith('MINE_TELEOP_')||/^(NODE_OPTIONS|ELECTRON_RUN_AS_NODE|LD_PRELOAD|LD_LIBRARY_PATH|DYLD_.*)$/.test(key))delete env[key];
  }
  if(platform==='linux')env.LD_LIBRARY_PATH=path.join(root,'lib');
  const ca=path.join(root,'certs','cacert.pem');
  if(fs.existsSync(ca))env.SSL_CERT_FILE=ca;
  return env;
}

class NativeController extends EventEmitter {
  constructor({root,logPath,startupMs=20000,shutdownMs=8000,spawnProcess=spawn,platform=process.platform}){
    super();Object.assign(this,{root,logPath,startupMs,shutdownMs,spawnProcess,platform});
    this.child=null;this.exited=false;this.stopping=false;this.stopPromise=null;
  }
  start(){
    if(this.child)throw Error('Controller already started');
    const executable=path.join(this.root,'bin',this.platform==='win32'?'mine-teleop-control.exe':'mine-teleop-control');
    return new Promise((resolve,reject)=>{
      let ready=false,settled=false,buffer='';
      const fail=error=>{if(!settled){settled=true;clearTimeout(timer);reject(error);}};
      const timer=setTimeout(()=>{fail(Error('控制服务启动超时'));void this.stop();},this.startupMs);
      try{
        this.child=this.spawnProcess(executable,['--config',path.join(this.root,'config','driver-console.yaml'),'--port','0','--desktop-managed','--no-open-browser','--browser-event-log',this.logPath],{
          cwd:this.root,env:controllerEnvironment(this.root,process.env,this.platform),windowsHide:true,stdio:['pipe','pipe','pipe'],shell:false
        });
      }catch(error){this.exited=true;fail(error);return;}
      this.child.stdin.on('error',()=>{}); // The child may exit before the shutdown write.
      this.child.stderr.on('data',()=>{}); // Always drain; never block the native control loop on a full pipe.
      this.child.stdout.on('data',chunk=>{
        buffer+=chunk.toString();
        if(buffer.length>65536){buffer='';return;}
        let newline;
        while((newline=buffer.indexOf('\n'))!==-1){
          const line=buffer.slice(0,newline);buffer=buffer.slice(newline+1);
          let value;try{value=JSON.parse(line);}catch{continue;}
          if(value.event!=='control_client_started'||ready||settled)continue;
          if(value.host!=='127.0.0.1'||!Number.isInteger(value.port)||value.port<1||value.port>65535||value.url!==`http://127.0.0.1:${value.port}/`){
            fail(Error('控制服务返回了无效的本地地址'));void this.stop();continue;
          }
          ready=true;settled=true;clearTimeout(timer);this.url=value.url;resolve(value.url);
        }
      });
      this.child.once('error',error=>{if(!ready)this.exited=true;fail(error);});
      this.child.once('exit',(code,signal)=>{
        this.exited=true;clearTimeout(timer);fail(Error(`控制服务已退出 (${code===null?signal:code})`));
        if(ready)this.emit('exit',{code,signal,expected:this.stopping});
      });
    });
  }
  stop(){
    if(this.stopPromise)return this.stopPromise;
    this.stopping=true;
    this.stopPromise=new Promise(resolve=>{
      if(!this.child||this.exited){resolve({forced:false});return;}
      let forced=false;
      const timer=setTimeout(()=>{forced=true;this.child.kill('SIGKILL');},this.shutdownMs);
      this.child.once('exit',()=>{clearTimeout(timer);resolve({forced});});
      this.child.stdin.end('shutdown\n');
    });
    return this.stopPromise;
  }
}
module.exports={NativeController,controllerEnvironment};
