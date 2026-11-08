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
async function backend({length=40,chunk=8,known=true,range=true,short=false,badRange=false,status=200,limit=64,
                        name="space #%.bin",body=null,failFirst=0,attempts=1,delay=0}={}) {
  const bytes = body || Uint8Array.from({length},(_,i)=>(i+1)%251);
  length = bytes.length;
  const requests=[];
  let inFlight=0, failures=failFirst;
  const later=(fn,ms)=>ms>=10000?0:setTimeout(fn,ms);
  const resident=[];
  const context={wasmFS$backends:{},HEAPU8:new Uint8Array(256),resident,
    _recomp_stream_resident:(path,start,end,size,on)=>resident.push([path,start,end,size,on]),
    stringToUTF8OnStack:s=>s,withStackSave:fn=>fn(),
    __wasmfs_fetch_get_file_url:()=>`https://example.test/prefix/data//${name}`,
    UTF8ToString:p=>p,_recomp_web_stream_size:()=>known?length:-1,
    __wasmfs_fetch_get_chunk_size:()=>chunk,console:{error(){},warn(){},log(){}},
    setTimeout:later,clearTimeout,setInterval:()=>0,AbortController,TextEncoder,crypto:globalThis.crypto,performance,
    fetch:async(url,opts={})=>{
      const request={url,method:opts.method||"GET",range:opts.headers?.Range,concurrent:++inFlight};
      requests.push(request);
      try{
        if(delay)await new Promise(r=>setTimeout(r,delay));
        if(failures>0){failures--;return {ok:false,status:503,headers:new Map(),body:{async cancel(){}}};}
        let data=bytes;
        let code=status;
        // A compressing proxy drops Content-Length; the manifest must not need it.
        const headers=new Map(name==="manifest.txt"?[]:[["Content-Length",String(length)]]);
        if(opts.headers?.Range && range && status===200){
          const [,start,end]=/bytes=(\d+)-(\d+)/.exec(opts.headers.Range);
          data=bytes.slice(Number(start),Number(end)+1); code=206;
          headers.set("Content-Range",badRange?"bytes 0-0/1":`bytes ${start}-${end}/${length}`);
        }
        if(short)data=data.slice(0,Math.max(0,data.length-1));
        return {ok:code>=200&&code<300,status:code,headers,body:{async cancel(){}},
          text:async()=>new TextDecoder().decode(data),
          arrayBuffer:async()=>data.buffer.slice(data.byteOffset,data.byteOffset+data.length)};
      }finally{inFlight--;}
    },
    addToLibrary:lib=>{context.lib=lib;},
  };
  vm.createContext(context);
  vm.runInContext(librarySource,context);
  context.recompStream=context.lib.$recompStream;
  Object.assign(context.recompStream,{chunk,memoryLimit:limit,attempts});
  await context.lib._wasmfs_create_fetch_backend_js(1);
  return {context,requests,bytes,api:context.wasmFS$backends[1],memory:()=>context.recompStream.memoryBytes||0};
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
  const {api,context,bytes,requests,memory}=await backend({length:96,limit:24});
  assert.equal(await api.read(1,0,19,5),19);
  assert.deepEqual(context.HEAPU8.slice(0,19),bytes.slice(5,24));
  for(let offset=24;offset<96;offset+=8){
    assert.equal(await api.read(1,0,8,offset),8);
    assert.ok(memory()<=24);
  }
  assert.equal(await api.read(1,0,8,0),8);
  assert.deepEqual(context.HEAPU8.slice(0,8),bytes.slice(0,8));
  assert.equal(await api.read(1,0,10,94),2);
  assert.equal(await api.read(1,0,10,96),0);
  assert.ok(requests.filter(r=>r.range==="bytes=0-7").length>=2);
});
test("a server ignoring Range is correct across later reads",async()=>{
  const {api,context,bytes,memory}=await backend({length:40,range:false});
  assert.equal(await api.read(1,0,10,5),10);
  assert.deepEqual(context.HEAPU8.slice(0,10),bytes.slice(5,15));
  assert.equal(await api.read(1,0,12,20),12);
  assert.deepEqual(context.HEAPU8.slice(0,12),bytes.slice(20,32));
  assert.ok(memory()<=64);
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
test("the manifest is sized from its body, so a proxy that drops Content-Length cannot break it",async()=>{
  const text="recomp-stream-v1\n12\tGAME.EXE\n";
  const {api,context,requests}=await backend({name:"manifest.txt",known:false,body:new TextEncoder().encode(text)});
  assert.equal(await api.getSize(1),text.length);
  assert.equal(await api.read(1,0,64,0),text.length);
  assert.equal(new TextDecoder().decode(context.HEAPU8.slice(0,text.length)),text);
  assert.deepEqual(requests.map(r=>r.method),["GET"]);
});
test("one read fetches all of its chunks at once and reads ahead",async()=>{
  const {api,context,bytes,requests}=await backend({length:96,delay:20});
  assert.equal(await api.read(1,0,24,0),24);
  assert.deepEqual(context.HEAPU8.slice(0,24),bytes.slice(0,24));
  assert.ok(Math.max(...requests.map(r=>r.concurrent))>=3, "chunks were fetched one after another");
  await new Promise(r=>setTimeout(r,60));
  const ranges=new Set(requests.map(r=>r.range));
  assert.ok(ranges.has("bytes=24-31") && ranges.has("bytes=32-39"), "no read-ahead past the read");
  assert.equal(await api.read(1,0,8,24),8);
  assert.deepEqual(context.HEAPU8.slice(0,8),bytes.slice(24,32));
  assert.equal(requests.filter(r=>r.range==="bytes=24-31").length,1, "read-ahead chunk was fetched again");
});
test("a prefetch hint fetches the named range's missing chunks once, by manifest name in any case",async()=>{
  const text="recomp-stream-v1\n40\tTRACKS/STREAM L2.BUN\n12\tGAME.EXE\n";
  const {api,context,requests}=await backend({name:"manifest.txt",known:false,chunk:8,
                                              body:new TextEncoder().encode(text)});
  assert.equal(context.recompStream.prefetch("tracks\\stream l2.bun",10,20),0,"hint before the manifest");
  assert.equal(await api.read(1,0,64,0),text.length);
  assert.equal(context.recompStream.prefetch("missing.bun",0,8),0);
  assert.equal(context.recompStream.prefetch("tracks\\stream l2.bun",10,20),3);
  assert.equal(context.recompStream.prefetch("TRACKS/STREAM L2.BUN",10,20),0,"queued twice");
  assert.equal(context.recompStream.prefetch("TRACKS/STREAM L2.BUN",36,100),1,"clamped to the file");
  await new Promise(r=>setTimeout(r,20));
  const ranges=requests.filter(r=>r.url==="https://example.test/prefix/data/TRACKS/STREAM%20L2.BUN").map(r=>r.range);
  assert.deepEqual(ranges.sort(),["bytes=16-23","bytes=24-31","bytes=32-39","bytes=8-15"]);
});
test("chunks entering and leaving memory are published to the runtime by manifest path",async()=>{
  // One file whose bytes are the manifest itself, so every fetch serves it.
  const head="recomp-stream-v1\n48\tTRACKS/A.BUN\n";
  const text=head+"x".repeat(48-head.length);
  const {api,context}=await backend({name:"manifest.txt",known:false,chunk:8,limit:24,
                                     body:new TextEncoder().encode(text)});
  assert.equal(await api.read(1,0,64,0),48);
  assert.deepEqual(context.resident,[],"the manifest itself is not a cached chunk");
  assert.equal(context.recompStream.prefetch("tracks/a.bun",0,48),6);
  await new Promise(r=>setTimeout(r,30));
  const on=context.resident.filter(r=>r[4]===1), off=context.resident.filter(r=>r[4]===0);
  assert.equal(on.length,6);
  assert.equal(off.length,3,"three chunks no longer fit in 24 bytes");
  for(const [path,start,end,size] of context.resident){
    assert.equal(path,"TRACKS/A.BUN");
    assert.equal(size,48);
    assert.equal(start%8,0);
    assert.equal(end,start+8);
  }
  // Whatever stayed resident was published last as resident.
  const last=new Map();
  for(const [,start,,,state] of context.resident) last.set(start,state);
  assert.equal([...last.values()].filter(v=>v===1).length,3);
});
test("a failed request is retried rather than failing the guest read",async()=>{
  const {api,context,bytes,requests}=await backend({length:12,failFirst:1,attempts:3});
  assert.equal(await api.read(1,0,12,0),12);
  assert.deepEqual(context.HEAPU8.slice(0,12),bytes);
  assert.equal(requests.length,2);
});
