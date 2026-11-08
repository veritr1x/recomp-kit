// setBindGroup for host/gpu/webgpu/d3d9_webgpu.cpp, which sets a bind group
// with four dynamic offsets for nearly every draw. Emscripten's binding passes
// the offsets as a view into the whole Wasm heap; this copies the requested
// words into a small array of their own and calls the typed-array overload.
// Firefox also rejects a dynamic-offset view whose backing memory can grow
// beyond 2 GiB, even when WebGPU reads only four words of it.
addToLibrary({
  recompWgpuRenderPassSetBindGroup__deps: ['$WebGPU'],
  recompWgpuRenderPassSetBindGroup: (passPtr, groupIndex, groupPtr, count, offsetsPtr) => {
    const pass = WebGPU.getJsObject(passPtr);
    const group = WebGPU.getJsObject(groupPtr);
    if (!count) {
      pass.setBindGroup(groupIndex, group);
      return;
    }
    const first = offsetsPtr >>> 2;
    const offsets = HEAPU32.slice(first, first + count);
    pass.setBindGroup(groupIndex, group, offsets, 0, count);
  },
});
