'use strict';
const {app,BrowserWindow,Menu,dialog,session}=require('electron');
const path=require('node:path');
const fs=require('node:fs');
const {NativeController}=require('./native-controller.cjs');

app.setName('MineTeleop');
const root=app.isPackaged?path.join(process.resourcesPath,'controller'):process.env.MINE_TELEOP_DESKTOP_ROOT;
let window=null,controller=null,quitting=false,finished=false;
if(!app.requestSingleInstanceLock())app.quit();
else{
  app.on('second-instance',()=>{if(window&&!window.isDestroyed()){if(window.isMinimized())window.restore();window.show();window.focus();}});
  app.on('before-quit',event=>{if(!finished){event.preventDefault();void shutdown();}});
  app.on('window-all-closed',()=>{void shutdown();});
  app.whenReady().then(start).catch(error=>{dialog.showErrorBox('MineTeleop 启动失败',error.message);void shutdown();});
}

async function shutdown(){
  if(quitting)return;quitting=true;
  // Destroy the renderer first: it can no longer submit fresh driving inputs.
  if(window&&!window.isDestroyed())window.destroy();
  try{if(controller){
    const result=await controller.stop();
    if(result.forced)fs.appendFileSync(path.join(app.getPath('userData'),'desktop-events.jsonl'),JSON.stringify({event:'native_shutdown_timeout',at:new Date().toISOString()})+'\n');
  }}catch(error){console.error('Desktop shutdown:',error.message);}
  finally{finished=true;app.quit();}
}

async function start(){
  if(!root)throw Error('开发模式需指定 MINE_TELEOP_DESKTOP_ROOT；正式包自动使用随包控制服务。');
  const data=app.getPath('userData');fs.mkdirSync(data,{recursive:true});
  controller=new NativeController({root:path.resolve(root),logPath:path.join(data,'control-browser-events.jsonl')});
  controller.on('exit',({expected})=>{if(!expected&&!quitting){dialog.showErrorBox('控制服务已停止','本地控制服务异常退出，窗口将关闭。请查看日志后重新启动。');void shutdown();}});
  const url=await controller.start();if(quitting)return;
  const origin=new URL(url).origin;
  session.defaultSession.setPermissionRequestHandler((contents,permission,callback)=>callback(permission==='fullscreen'&&contents.getURL().startsWith(origin+'/')));
  session.defaultSession.setPermissionCheckHandler((contents,permission,requestingOrigin)=>permission==='fullscreen'&&requestingOrigin===origin);
  window=new BrowserWindow({title:'MineTeleop 控制台',width:1600,height:1000,minWidth:800,minHeight:600,show:false,backgroundColor:'#08111d',autoHideMenuBar:true,
    webPreferences:{nodeIntegration:false,contextIsolation:true,sandbox:true,webSecurity:true,backgroundThrottling:true,spellcheck:false}
  });
  const menu=Menu.buildFromTemplate([
    ...(process.platform==='darwin'?[{label:'MineTeleop',submenu:[{label:'退出 MineTeleop',accelerator:'Cmd+Q',click:()=>void shutdown()}]}]:[]),
    {label:'窗口',submenu:[{role:'togglefullscreen'},{role:'minimize'},{label:'退出控制台',click:()=>void shutdown()}]}
  ]);
  Menu.setApplicationMenu(menu);
  window.on('close',event=>{event.preventDefault();void shutdown();});
  window.webContents.setWindowOpenHandler(()=>({action:'deny'}));
  window.webContents.on('will-navigate',(event,target)=>{if(target!==url)event.preventDefault();});
  window.webContents.on('will-attach-webview',event=>event.preventDefault());
  window.webContents.on('render-process-gone',()=>{if(!quitting){dialog.showErrorBox('控制台页面已停止','页面进程异常退出，控制服务将同步关闭。');void shutdown();}});
  window.webContents.on('will-prevent-unload',event=>event.preventDefault());
  await window.loadURL(url);
  if(!quitting){window.maximize();window.show();}
}
