'use strict';
const fs=require('node:fs');
const path=require('node:path');

function prepareLogDirectory({projectRoot,execPath=process.execPath,platform=process.platform,getFallbackDirectory,fileSystem=fs}){
  const primary=path.join(projectRoot,'log');
  const prepare=directory=>{
    fileSystem.mkdirSync(directory,{recursive:true});
    // mkdir also succeeds for an existing read-only directory. Check the
    // directory (rotation needs it) and log before starting the native service.
    fileSystem.accessSync(directory,fs.constants.W_OK);
    // Append mode preserves existing events.
    const fd=fileSystem.openSync(path.join(directory,'control-browser-events.jsonl'),'a');
    fileSystem.closeSync(fd);
    return directory;
  };
  const translocated=platform==='darwin'&&/\/AppTranslocation\/[^/]+\/d\//.test(execPath);
  if(!translocated){
    try{return prepare(primary);}
    catch(error){
      if(!['EACCES','EPERM','EROFS','ENOENT','ENOTDIR'].includes(error.code))throw error;
    }
  }
  // Never write into macOS's randomized read-only translocation mount.
  // Resolve lazily so normal portable installs need no user-directory writes.
  return prepare(getFallbackDirectory());
}

module.exports={prepareLogDirectory};
