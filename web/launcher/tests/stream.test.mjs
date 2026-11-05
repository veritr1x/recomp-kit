import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";
import { selectSource, streamManifestUrl } from "../../player/source.mjs";

const game = {id:"sample",title:"Sample",executable:"Game.exe",exeDir:"Binaries/Win32",
              streamManifest:"./sample/data/manifest.txt"};
const gamesUrl = "https://example.test/prefix/games.json";
test("import and stream selection preserve the import route and assigned profile", async () => {
  const unavailable = async () => { throw new Error("must not fetch"); };
  assert.equal((await selectSource(game,gamesUrl,"",true,unavailable)).kind,"import");
  assert.equal((await selectSource(game,gamesUrl,"?source=import",true,unavailable)).args[0],
               "--exe=/opfs/sample/game/Binaries/Win32/Game.exe");
  const hosted = await selectSource(game,gamesUrl,"?source=stream",true,async()=>({ok:true}));
  assert.deepEqual(hosted.args,["--exe=/data/Binaries/Win32/Game.exe","--stream-base","https://example.test/prefix/sample/data"]);
  assert.equal((await selectSource(game,gamesUrl,"",false,async()=>({ok:true}))).kind,"stream");
  await assert.rejects(selectSource({...game,streamManifest:undefined},gamesUrl,"",false),/not been imported/);
  await assert.rejects(selectSource(game,gamesUrl,"?source=import",false,unavailable),/not been imported/);
  await assert.rejects(selectSource(game,gamesUrl,"?source=stream",false,async()=>({ok:false,status:404})),/unavailable/);
  await assert.rejects(selectSource(game,gamesUrl,"",false,async()=>({ok:false,status:404})),/not been imported/);
  assert.throws(()=>streamManifestUrl({...game,streamManifest:"https://other.test/data/manifest.txt"},gamesUrl));
});

const librarySource = fs.readFileSync(new URL("../../player/fetch-backend.js", import.meta.url),"utf8")
  .replaceAll("{{{ cDefs.EROFS }}}","30").replaceAll("{{{ cDefs.ENOENT }}}","2").replaceAll("{{{ cDefs.EIO }}}","5");
async function backend({length=40,chunk=8,known=true,range=true,short=false,badRange=false,status=200,limit=64}={}) {
  const bytes = Uint8Array.from({length},(_,i)=>(i+1)%251);
  const requests=[];
  const context={wasmFS$backends:{},HEAPU8:new Uint8Array(256),
    __wasmfs_fetch_get_file_url:()=>"https://example.test/prefix/data//space #%.bin",
    UTF8ToString:p=>p,_recomp_web_stream_size:()=>known?length:-1,
    __wasmfs_fetch_get_chunk_size:()=>chunk,console:{error(){}},
    fetch:async(url,opts={})=>{
      requests.push({url,method:opts.method||"GET",range:opts.headers?.Range});
      let data=bytes;
      let code=status;
      const headers=new Map([["Content-Length",String(length)]]);
      if(opts.headers?.Range && range && status===200){
        const [,start,end]=/bytes=(\d+)-(\d+)/.exec(opts.headers.Range);
        data=bytes.slice(Number(start),Number(end)+1); code=206;
        headers.set("Content-Range",badRange?"bytes 0-0/1":`bytes ${start}-${end}/${length}`);
      }
      if(short)data=data.slice(0,Math.max(0,data.length-1));
      return {ok:code>=200&&code<300,status:code,headers,body:{async cancel(){}},
        arrayBuffer:async()=>data.buffer.slice(data.byteOffset,data.byteOffset+data.length)};
    },
    addToLibrary:lib=>{context.lib=lib;},
  };
  vm.createContext(context);
  vm.runInContext(librarySource,context);
  context.recompFetchCache=context.lib.$recompFetchCache;
  context.recompFetchCache.limit=limit;
  await context.lib._wasmfs_create_fetch_backend_js(1);
  return {context,requests,bytes,api:context.wasmFS$backends[1]};
}
test("whole-file cross-chunk reads retain every byte (SDK #28001)",async()=>{
  const {api,context,bytes,requests}=await backend({length:12});
  assert.equal(await api.read(1,0,4,0),4);
  assert.equal(await api.read(1,4,8,4),8);
  assert.deepEqual(context.HEAPU8.slice(0,12),bytes);
  assert.equal(requests.length,1);
  assert.equal(requests[0].url,"https://example.test/prefix/data/space%20%23%25.bin");
});
test("manifest sizes do not fetch assets during directory indexing",async()=>{
  const {api,requests}=await backend();
  assert.equal(await api.getSize(1),40);
  assert.equal(requests.length,0);
});
test("range reads cross boundaries, clamp EOF and refetch evicted chunks",async()=>{
  const {api,context,bytes,requests}=await backend({length:96,limit:24});
  assert.equal(await api.read(1,0,19,5),19);
  assert.deepEqual(context.HEAPU8.slice(0,19),bytes.slice(5,24));
  for(let offset=24;offset<96;offset+=8){
    assert.equal(await api.read(1,0,8,offset),8);
    assert.ok(context.recompFetchCache.bytes<=24);
  }
  assert.equal(await api.read(1,0,8,0),8);
  assert.deepEqual(context.HEAPU8.slice(0,8),bytes.slice(0,8));
  assert.equal(await api.read(1,0,10,94),2);
  assert.equal(await api.read(1,0,10,96),0);
  assert.ok(requests.filter(r=>r.range==="bytes=0-7").length>=2);
});
test("a server ignoring Range is correct across later reads",async()=>{
  const {api,context,bytes}=await backend({length:40,range:false});
  assert.equal(await api.read(1,0,10,5),10);
  assert.deepEqual(context.HEAPU8.slice(0,10),bytes.slice(5,15));
  assert.equal(await api.read(1,0,12,20),12);
  assert.deepEqual(context.HEAPU8.slice(0,12),bytes.slice(20,32));
  assert.ok(context.recompFetchCache.bytes<=64);
});
test("unknown-size manifest uses HEAD and small empty files work",async()=>{
  const {api,requests}=await backend({length:12,known:false});
  assert.equal(await api.getSize(1),12);
  assert.equal(await api.read(1,0,12,0),12);
  assert.deepEqual(requests.map(r=>r.method),["HEAD","GET"]);
  const empty=await backend({length:0});
  assert.equal(await empty.api.read(1,0,10,0),0);
});
test("missing/truncated responses and malformed ranges fail instead of reporting valid bytes",async()=>{
  for(const options of [{short:true},{badRange:true},{length:12,short:true},{length:96,range:false,limit:24}]){
    const {api}=await backend(options);
    assert.equal(await api.read(1,0,12,0),-5);
  }
  const {api}=await backend({status:404});
  assert.equal(await api.read(1,0,10,0),-2);
  assert.equal(await api.write(1,0,1,0),-30);
});
