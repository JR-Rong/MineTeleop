#!/usr/bin/env node
import {packager} from '../../desktop/node_modules/@electron/packager/dist/index.js';
import {cp,mkdir,readFile,writeFile,mkdtemp,rm,stat,readdir} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {execFileSync} from 'node:child_process';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
const repo=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const args=Object.fromEntries(process.argv.slice(2).map((value,index,all)=>value.startsWith('--')?[value.slice(2),all[index+1]]:null).filter(Boolean));
const platform=args.platform,arch=args.arch,nativeRoot=args['native-root']&&path.resolve(args['native-root']);
if(!['darwin','win32','linux'].includes(platform)||!['arm64','x64'].includes(arch)||!nativeRoot)throw Error('usage: node scripts/build/build_desktop_control_bundle.mjs --platform darwin|win32|linux --arch arm64|x64 --native-root /unpacked/native/bundle [--output /directory]');
if(platform==='linux'&&process.platform!=='linux')throw Error('Linux final packaging must run on Linux to collect runtime dependencies');
const sourceCommit=args['source-commit']||execFileSync('git',['rev-parse','HEAD'],{cwd:repo,encoding:'utf8'}).trim();
if(!/^[0-9a-f]{40}$/.test(sourceCommit))throw Error('source-commit must be a full Git SHA');
const sourceDirty=args['source-dirty']===undefined?Boolean(execFileSync('git',['status','--porcelain'],{cwd:repo,encoding:'utf8'}).trim()):args['source-dirty']==='true';
const output=path.resolve(args.output||path.join(repo,'dist/desktop'));
const executable='bin/mine-teleop-control'+(platform==='win32'?'.exe':'');
for(const file of [executable,'config/driver-console.yaml','config/mine-teleop-field-root.crt','assets/console-layout.js','assets/scene.js','assets/models/haul-truck/model.json'])await stat(path.join(nativeRoot,file));
const nativeBytes=await readFile(path.join(nativeRoot,executable));
if(!nativeBytes.includes(Buffer.from('--desktop-managed')))throw Error('Native bundle is too old: missing desktop owner-pipe support');
let nativeMachine;
if(platform==='win32'&&nativeBytes.toString('ascii',0,2)==='MZ'){
  const pe=nativeBytes.readUInt32LE(60);
  if(pe+6<=nativeBytes.length&&nativeBytes.readUInt32LE(pe)===0x4550)nativeMachine=nativeBytes.readUInt16LE(pe+4);
  if(nativeMachine!==(arch==='x64'?0x8664:0xaa64))throw Error('Native PE architecture does not match desktop target');
}else if(platform==='linux'&&nativeBytes.toString('hex',0,6)==='7f454c460201'){
  if(nativeBytes.readUInt16LE(18)!==(arch==='x64'?62:183))throw Error('Native ELF architecture does not match desktop target');
}else if(platform==='darwin'&&nativeBytes.readUInt32LE(0)===0xfeedfacf){
  if(nativeBytes.readUInt32LE(4)!==(arch==='x64'?0x1000007:0x100000c))throw Error('Native Mach-O architecture does not match desktop target');
}else throw Error('Native executable format does not match desktop platform');
const temporary=await mkdtemp(path.join(os.tmpdir(),'mine-desktop-'));
try{
  const appSource=path.join(temporary,'app'),controller=path.join(temporary,'controller');
  await mkdir(appSource);
  for(const file of ['main.cjs','native-controller.cjs','package.json'])await cp(path.join(repo,'desktop',file),path.join(appSource,file));
  const packageJSON=JSON.parse(await readFile(path.join(appSource,'package.json'),'utf8'));
  const electronVersion=packageJSON.devDependencies.electron;delete packageJSON.devDependencies;delete packageJSON.scripts;
  await writeFile(path.join(appSource,'package.json'),JSON.stringify(packageJSON,null,2));
  await mkdir(controller);
  // An explicit allowlist keeps logs, credentials, local work, and build trees out of release artifacts.
  for(const directory of ['bin','lib','config','certs','assets','protocol']){
    try{await stat(path.join(nativeRoot,directory));}catch{continue;}
    await cp(path.join(nativeRoot,directory),path.join(controller,directory),{recursive:true,filter:source=>!path.basename(source).startsWith('._')&&!['.local','node_modules'].includes(path.basename(source))});
  }
  await cp(path.resolve(args.config||path.join(repo,'configs/driver-console.three-machine.dev.yaml')),path.join(controller,'config/driver-console.yaml'));
  const packages=await packager({dir:appSource,name:'MineTeleop',executableName:'MineTeleop',platform,arch,electronVersion,out:output,overwrite:false,asar:true,prune:true,extraResource:[controller],appBundleId:'com.mineteleop.control',appCategoryType:'public.app-category.business'});
  for(const folder of packages){
    if(platform==='darwin'&&process.platform==='darwin')execFileSync('codesign',['--force','--deep','--sign','-',path.join(folder,'MineTeleop.app')]);
    if(platform==='linux'){
      if(process.platform!=='linux')throw Error('Linux final packaging must run on Linux to collect and verify runtime dependencies');
      execFileSync('python3',[path.join(repo,'scripts/build/collect_linux_desktop_runtime.py'),folder],{stdio:'inherit'});
    }
    const manifest=[];
    async function collect(directory){for(const entry of await readdir(directory,{withFileTypes:true})){const file=path.join(directory,entry.name);if(entry.isDirectory())await collect(file);else if(entry.isFile())manifest.push({path:path.relative(folder,file),bytes:(await stat(file)).size,sha256:createHash('sha256').update(await readFile(file)).digest('hex')});}}
    await collect(folder);
    await writeFile(path.join(folder,'DESKTOP-BUILD.json'),JSON.stringify({platform,arch,electron:electronVersion,source_commit:sourceCommit,source_dirty:sourceDirty,built_at:new Date().toISOString(),files:manifest},null,2));
    const archive=folder+(platform==='linux'?'.tar.gz':'.zip');
    if(platform==='linux')execFileSync('tar',['-czf',archive,'-C',path.dirname(folder),path.basename(folder)]);
    else if(process.platform==='darwin')execFileSync('ditto',['-c','-k','--sequesterRsrc','--keepParent',folder,archive]);
    else if(process.platform==='win32')execFileSync('powershell.exe',['-NoProfile','-NonInteractive','-Command','Compress-Archive -LiteralPath $env:MINE_DESKTOP_FOLDER -DestinationPath $env:MINE_DESKTOP_ARCHIVE'],{env:{...process.env,MINE_DESKTOP_FOLDER:folder,MINE_DESKTOP_ARCHIVE:archive}});
    else execFileSync('zip',['-qr',archive,path.basename(folder)],{cwd:path.dirname(folder)});
    const bytes=await readFile(archive);await writeFile(archive+'.sha256',createHash('sha256').update(bytes).digest('hex')+'  '+path.basename(archive)+'\n');
    console.log(JSON.stringify({desktop_bundle:archive,bytes:bytes.length,uncompressed_bytes:manifest.reduce((sum,file)=>sum+file.bytes,0)}));
  }
}finally{await rm(temporary,{recursive:true,force:true});}
