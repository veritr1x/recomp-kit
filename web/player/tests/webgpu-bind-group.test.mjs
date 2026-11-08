// node --test web/player/tests - the WebGPU setBindGroup library function.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('../webgpu-bind-group.js', import.meta.url), 'utf8');

test('dynamic offsets go as a small array of their own, not a view of the heap', () => {
  let library, received;
  const heap = {
    buffer: { byteLength: 4 * 1024 ** 3 },
    slice(first, end) {
      assert.deepEqual([first, end], [3, 7]);
      return new Uint32Array([16, 32, 48, 64]);
    },
  };
  const pass = {
    setBindGroup(index, group, offsets, start, length) {
      assert.equal(index, 2);
      assert.equal(group, 'group');
      assert.deepEqual([start, length], [0, 4]);
      if (offsets?.buffer?.byteLength > 2 * 1024 ** 3)
        throw new RangeError('a view backed by more than 2 GiB');
      received = offsets;
    },
  };
  const context = {
    HEAPU32: heap,
    WebGPU: { getJsObject: pointer => (pointer === 1 ? pass : 'group') },
    addToLibrary: value => { library = value; },
  };
  vm.runInNewContext(source, context);
  library.recompWgpuRenderPassSetBindGroup(1, 2, 3, 4, 12);
  assert.deepEqual([...received], [16, 32, 48, 64]);
  assert.equal(received.buffer.byteLength, 16);
});

test('a bind group without dynamic offsets keeps the short call', () => {
  let library, args;
  const pass = { setBindGroup(...value) { args = value; } };
  vm.runInNewContext(source, {
    HEAPU32: { slice() { throw new Error('must not read offsets'); } },
    WebGPU: { getJsObject: pointer => (pointer === 1 ? pass : 'group') },
    addToLibrary: value => { library = value; },
  });
  library.recompWgpuRenderPassSetBindGroup(1, 0, 2, 0, 0);
  assert.deepEqual(args, [0, 'group']);
});
