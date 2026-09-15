'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {once}=require('node:events');
const os=require('node:os');
const path=require('node:path');
const {NativeController}=require('../native-controller.cjs');
const root=process.env.MINE_TELEOP_TEST_NATIVE_ROOT;
for(const mode of ['shutdown','owner-eof',...(process.platform==='win32'?[]:['sigterm'])]){
  test(`real bundled service releases its port on ${mode}`,{skip:!root,timeout:15000},async t=>{
    const controller=new NativeController({root,logPath:path.join(os.tmpdir(),'mine-desktop-lifecycle-test.jsonl')});
    t.after(()=>controller.stop());
    const url=await controller.start();
    assert.equal((await (await fetch(url+'health')).json()).status,'ok');
    assert.equal((await (await fetch(url+'api/status')).json()).authenticated,false);
    const exit=once(controller,'exit');
    if(mode==='shutdown')assert.equal((await controller.stop()).forced,false);
    else if(mode==='owner-eof')controller.child.stdin.end();
    else controller.child.kill('SIGTERM');
    const [result]=await exit;
    assert.equal(result.code,0);
    await assert.rejects(fetch(url+'health'));
  });
}
