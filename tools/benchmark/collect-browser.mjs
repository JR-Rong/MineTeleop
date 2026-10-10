// Run only in the isolated test bench with media_frame_trace=true. This collector
// joins late sidecars by their original display time; unmatched frames stay gaps.
import {createWriteStream} from 'node:fs';
import {createRequire} from 'node:module';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const [url,out]=process.argv.slice(2);if(!url||!out)throw Error('Usage: collect-browser.mjs URL OUTPUT.jsonl');
const sink=createWriteStream(out,{flags:'wx'}),browser=await chromium.launch({headless:false,args:['--disable-gpu','--disable-accelerated-video-decode']});
const page=await browser.newPage();let dropped=0;
await page.exposeBinding('benchFrame',(_,detail)=>{if(sink.writableLength>1024*1024){dropped++;return;}sink.write(JSON.stringify({event:'browser_frame',...detail})+'\n');});
await page.addInitScript(()=>document.addEventListener('media-frame-presented',event=>globalThis.benchFrame(event.detail),true));
await page.goto(url);console.log('Collector attached. GPU flags are not evidence of software decode; inspect actual decoder stats.');
await new Promise(resolve=>process.once('SIGINT',resolve));sink.end(JSON.stringify({event:'collector_dropped',dropped})+'\n');await browser.close();
