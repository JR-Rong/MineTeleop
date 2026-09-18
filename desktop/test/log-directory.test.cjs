'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const os=require('node:os');
const path=require('node:path');
const {prepareLogDirectory}=require('../log-directory.cjs');

function fixture(t){
  const root=fs.mkdtempSync(path.join(os.tmpdir(),'mine-log-path-'));
  t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
  return {projectRoot:path.join(root,'installation'),fallback:path.join(root,'user-logs')};
}

test('writable installation keeps root/log and preserves existing events',t=>{
  const {projectRoot}=fixture(t);
  const options={projectRoot,getFallbackDirectory:()=>assert.fail('unnecessary fallback')};
  const directory=prepareLogDirectory(options);
  assert.equal(directory,path.join(projectRoot,'log'));
  const log=path.join(directory,'control-browser-events.jsonl');
  fs.appendFileSync(log,'existing event\n');
  assert.equal(prepareLogDirectory(options),directory);
  assert.equal(fs.readFileSync(log,'utf8'),'existing event\n');
});

test('macOS translocation goes directly to persistent user logs',t=>{
  const {fallback}=fixture(t);
  const projectRoot='/private/var/folders/9h/test/T/AppTranslocation/DEC04F85/d';
  const touched=[];
  const fileSystem={...fs,mkdirSync:(directory,options)=>{
    touched.push(directory);
    assert.equal(directory,fallback,'attempted to write into translocation mount');
    return fs.mkdirSync(directory,options);
  }};
  const directory=prepareLogDirectory({projectRoot,platform:'darwin',
    execPath:projectRoot+'/MineTeleop.app/Contents/MacOS/MineTeleop',
    getFallbackDirectory:()=>fallback,fileSystem});
  assert.equal(directory,fallback);
  assert.deepEqual(touched,[fallback]);
  assert.ok(fs.existsSync(path.join(directory,'control-browser-events.jsonl')));
});

for(const platform of ['darwin','win32','linux']){
  for(const code of ['EACCES','EPERM','EROFS','ENOENT','ENOTDIR']){
    test(`${platform}: ${code} at installation falls back to writable user logs`,t=>{
      const {projectRoot,fallback}=fixture(t);
      const fileSystem={...fs,mkdirSync:(directory,options)=>{
        if(directory===path.join(projectRoot,'log'))throw Object.assign(new Error(code),{code});
        return fs.mkdirSync(directory,options);
      }};
      assert.equal(prepareLogDirectory({projectRoot,platform,execPath:'/normal/app',
        getFallbackDirectory:()=>fallback,fileSystem}),fallback);
    });
  }
}

test('existing directory without rotation permission falls back',t=>{
  const {projectRoot,fallback}=fixture(t);
  const fileSystem={...fs,accessSync:(directory,mode)=>{
    if(directory===path.join(projectRoot,'log'))throw Object.assign(new Error('denied'),{code:'EACCES'});
    return fs.accessSync(directory,mode);
  }};
  assert.equal(prepareLogDirectory({projectRoot,getFallbackDirectory:()=>fallback,fileSystem}),fallback);
});

test('existing log without append permission also falls back',t=>{
  const {projectRoot,fallback}=fixture(t);
  const fileSystem={...fs,openSync:(file,flags)=>{
    if(file===path.join(projectRoot,'log','control-browser-events.jsonl')){
      throw Object.assign(new Error('denied'),{code:'EACCES'});
    }
    return fs.openSync(file,flags);
  }};
  assert.equal(prepareLogDirectory({projectRoot,getFallbackDirectory:()=>fallback,fileSystem}),fallback);
});

test('disk errors and an unwritable fallback still fail explicitly',t=>{
  const {projectRoot,fallback}=fixture(t);
  const full=Object.assign(new Error('disk full'),{code:'ENOSPC'});
  assert.throws(()=>prepareLogDirectory({projectRoot,
    getFallbackDirectory:()=>assert.fail('disk error hidden'),
    fileSystem:{mkdirSync:()=>{throw full;}}}),error=>error===full);
  const denied=Object.assign(new Error('fallback denied'),{code:'EACCES'});
  assert.throws(()=>prepareLogDirectory({projectRoot,getFallbackDirectory:()=>fallback,
    fileSystem:{mkdirSync:()=>{throw denied;}}}),error=>error===denied);
});
