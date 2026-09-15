import {build} from 'esbuild';
import {mkdir,copyFile,writeFile,readFile,rm} from 'node:fs/promises';
await mkdir('assets',{recursive:true});
await rm('assets/chunks',{recursive:true,force:true});
await build({entryPoints:['scene.mjs'],bundle:true,splitting:true,format:'esm',minify:true,target:'es2022',outdir:'assets',entryNames:'[name]',chunkNames:'chunks/[name]-[hash]',legalComments:'eof'});
await copyFile('console_layout.js','assets/console-layout.js');
await copyFile('console_layout.css','assets/console-layout.css');
await writeFile('assets/THIRD-PARTY-LICENSES.txt','Three.js 0.186.0\n'+await readFile('node_modules/three/LICENSE','utf8'));
