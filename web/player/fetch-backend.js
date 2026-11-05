/**
 * @license
 * Copyright 2022 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 * Derived from Emscripten 6.0.11's libwasmfs_fetch.js. See FETCH-NOTICE.txt.
 * Keep this replacement in the app, without modifying the installed SDK.
 */
addToLibrary({
  $recompFetchCache: { entries: null, bytes: 0, limit: 64 * 1024 * 1024 },
  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends', '$recompFetchCache',
    '_wasmfs_fetch_get_file_url', '_wasmfs_fetch_get_chunk_size', 'recomp_web_stream_size',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    const cache = recompFetchCache;
    if (!cache.entries) cache.entries = new Map();
    const sizes = new Map();
    const pending = new Map();
    function cached(key) {
      const bytes = cache.entries.get(key);
      if (bytes) { cache.entries.delete(key); cache.entries.set(key, bytes); }
      return bytes;
    }
    function retain(key, bytes) {
      const previous = cache.entries.get(key);
      if (previous) { cache.bytes -= previous.length; cache.entries.delete(key); }
      while (cache.bytes + bytes.length > cache.limit && cache.entries.size) {
        const oldest = cache.entries.keys().next().value;
        cache.bytes -= cache.entries.get(oldest).length;
        cache.entries.delete(oldest);
      }
      if (bytes.length <= cache.limit) {
        cache.entries.set(key, bytes);
        cache.bytes += bytes.length;
      }
      return bytes;
    }
    async function load(key, fetchBytes) {
      const have = cached(key);
      if (have) return have;
      if (!pending.has(key))
        pending.set(key, fetchBytes().then(bytes => retain(key, bytes)).finally(() => pending.delete(key)));
      return pending.get(key);
    }
    function fileInfo(file) {
      const pointer = __wasmfs_fetch_get_file_url(file);
      const raw = UTF8ToString(pointer);
      // FetchDirectory joins a raw child path to the already encoded base URL
      // with two slashes. Escape child names (including # and %) only once.
      const seam = raw.indexOf('//', raw.indexOf('://') + 3);
      const url = seam < 0 ? raw : raw.slice(0, seam) + '/' +
        raw.slice(seam + 2).split('/').map(encodeURIComponent).join('/');
      return { url, size: _recomp_web_stream_size(pointer), chunk: __wasmfs_fetch_get_chunk_size(file) };
    }
    async function sizeOf(info) {
      if (info.size >= 0) return info.size;
      if (!sizes.has(info.url)) {
        const response = await fetch(info.url, { method: 'HEAD' });
        if (!response.ok) throw response;
        const value = response.headers.get('Content-Length');
        const size = value === null ? NaN : Number(value);
        if (!Number.isSafeInteger(size) || size < 0) throw new Error('Missing asset size');
        sizes.set(info.url, size);
      }
      return sizes.get(info.url);
    }
    async function whole(response, size) {
      if (!response.ok) throw response;
      // Large assets require Range support. Never accumulate an unbounded
      // whole-file response in JS memory when a server ignores the request.
      if (size > cache.limit) {
        if (response.body) await response.body.cancel();
        throw new Error('Large streamed assets require HTTP byte ranges');
      }
      const bytes = new Uint8Array(await response.arrayBuffer());
      if (bytes.length !== size) throw new Error('Truncated streamed asset');
      return bytes;
    }
    wasmFS$backends[backend] = {
      allocFile: async () => {},
      freeFile: async file => { sizes.delete(fileInfo(file).url); },
      write: async () => -{{{ cDefs.EROFS }}},
      read: async (file, buffer, length, offset) => {
        if (offset < 0 || length <= 0) return 0;
        try {
          const info = fileInfo(file);
          const size = await sizeOf(info);
          length = Math.min(length, size - offset);
          if (length <= 0) return 0;
          const wholeKey = info.url + '|whole';
          let complete = cached(wholeKey);
          if (!complete && size <= Math.min(2 * info.chunk, cache.limit))
            complete = await load(wholeKey, async () => whole(await fetch(info.url), size));
          if (complete) {
            // Whole-file bytes always use file offsets, never backend chunk
            // indexes. This fixes the cross-chunk corruption in SDK #28001.
            HEAPU8.set(complete.subarray(offset, offset + length), buffer);
            return length;
          }
          const end = offset + length;
          for (let at = offset; at < end;) {
            const start = Math.floor(at / info.chunk) * info.chunk;
            const last = Math.min(start + info.chunk, size) - 1;
            const bytes = await load(info.url + '|' + start, async () => {
              const response = await fetch(info.url, { headers: { Range: `bytes=${start}-${last}` } });
              if (response.status === 200) {
                const all = retain(wholeKey, await whole(response, size));
                return all.slice(start, last + 1);
              }
              if (response.status !== 206) throw response;
              const range = response.headers.get('Content-Range');
              if (range !== `bytes ${start}-${last}/${size}`) throw new Error('Invalid asset range');
              const part = new Uint8Array(await response.arrayBuffer());
              if (part.length !== last - start + 1) throw new Error('Truncated asset range');
              return part;
            });
            const count = Math.min(end - at, last + 1 - at);
            HEAPU8.set(bytes.subarray(at - start, at - start + count), buffer + at - offset);
            at += count;
          }
          return length;
        } catch (error) {
          console.error('[stream] asset read failed:', fileInfo(file).url, error.message || error.status);
          return error.status === 404 ? -{{{ cDefs.ENOENT }}} : -{{{ cDefs.EIO }}};
        }
      },
      getSize: async file => {
        try { return await sizeOf(fileInfo(file)); }
        catch { return 0; }
      },
    };
  },
});
