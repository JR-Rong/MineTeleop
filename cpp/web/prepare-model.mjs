// Offline asset preparation. Original FBX is never modified. Run from cpp/web.
import {readFile,writeFile,mkdir,stat} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import path from 'node:path';
import * as THREE from 'three';
import {FBXLoader} from 'three/addons/loaders/FBXLoader.js';
import {Document,NodeIO} from '@gltf-transform/core';
import {weld,simplify,prune} from '@gltf-transform/functions';
import {MeshoptSimplifier} from 'meshoptimizer';
import sharp from 'sharp';

const [input,output]=process.argv.slice(2);
if(!input||!output)throw Error('usage: node prepare-model.mjs source.fbx output.glb');
const bytes=await readFile(input);
const manager=new THREE.LoadingManager();
manager.addHandler(/\.(png|jpe?g|webp)$/i,{path:'',setPath(value){this.path=value;return this;},load(url){const texture=new THREE.Texture();texture.userData.sourceURL=url;return texture;}});
const object=new FBXLoader(manager).parse(bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength),'');
object.updateMatrixWorld(true);
const document=new Document(),buffer=document.createBuffer(),scene=document.createScene('Vehicle');
const box=new THREE.Box3().setFromObject(object);
const materials=new Map();
async function convertTexture(texture,limit){
  const url=texture?.userData.sourceURL;if(!url)return null;
  const source=Buffer.from(await (await fetch(url)).arrayBuffer());
  const image=await sharp(source).resize({width:limit,height:limit,fit:'inside',withoutEnlargement:true}).png().toBuffer();
  return document.createTexture(texture.name).setImage(image).setMimeType('image/png');
}
const meshes=[];object.traverse(node=>{if(node.isMesh)meshes.push(node);});
let inputTriangles=0;
for(const mesh of meshes){
  const geometry=mesh.geometry.clone().applyMatrix4(mesh.matrixWorld);
  const sourceMaterial=Array.isArray(mesh.material)?mesh.material[0]:mesh.material;
  if(Array.isArray(mesh.material)&&mesh.material.length>1)throw Error('Offline converter requires one material per mesh; use direct FBX loading for multi-material models.');
  if(!materials.has(sourceMaterial)){
    const material=document.createMaterial(sourceMaterial.name).setBaseColorFactor([1,1,1,1]).setRoughnessFactor(0.8).setMetallicFactor(0);
    const color=await convertTexture(sourceMaterial.map,2048);if(color)material.setBaseColorTexture(color);
    const normal=await convertTexture(sourceMaterial.normalMap,1024);if(normal)material.setNormalTexture(normal);
    materials.set(sourceMaterial,material);
  }
  const primitive=document.createPrimitive().setMaterial(materials.get(sourceMaterial));
  for(const [key,semantic,type] of [['position','POSITION','VEC3'],['normal','NORMAL','VEC3'],['uv','TEXCOORD_0','VEC2']]){
    const attribute=geometry.getAttribute(key);if(!attribute)continue;
    const data=new Float32Array(attribute.array);
    if(key==='uv')for(let i=1;i<data.length;i+=2)data[i]=1-data[i];
    primitive.setAttribute(semantic,document.createAccessor().setType(type).setArray(data).setBuffer(buffer));
  }
  const indices=geometry.index?new Uint32Array(geometry.index.array):Uint32Array.from({length:geometry.getAttribute('position').count},(_,i)=>i);
  inputTriangles+=indices.length/3;
  primitive.setIndices(document.createAccessor().setType('SCALAR').setArray(indices).setBuffer(buffer));
  scene.addChild(document.createNode(mesh.name).setMesh(document.createMesh().addPrimitive(primitive)));
}
await document.transform(weld(),simplify({simplifier:MeshoptSimplifier,ratio:0.18,error:0.002}),prune());
await mkdir(path.dirname(output),{recursive:true});
await new NodeIO().write(output,document);
const report={source:path.basename(input),source_sha256:createHash('sha256').update(bytes).digest('hex'),source_bytes:bytes.length,output_bytes:(await stat(output)).size,input_triangles:inputTriangles,output_triangles:document.getRoot().listMeshes().reduce((sum,m)=>sum+m.listPrimitives().reduce((n,p)=>n+p.getIndices().getCount()/3,0),0),source_bounds:{min:box.min.toArray(),max:box.max.toArray()},base_color_max_pixels:2048,normal_max_pixels:1024,simplification_error:0.002};
await writeFile(output+'.provenance.json',JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify(report,null,2));
