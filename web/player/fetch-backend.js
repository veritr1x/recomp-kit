/**
 * @license
 * Copyright 2022 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 * Derived from Emscripten 6.0.11's libwasmfs_fetch.js. See FETCH-NOTICE.txt.
 * Keep this replacement in the app, without modifying the installed SDK.
 *
 * A guest read blocks its thread until every byte is here, so over the
 * internet each miss costs a full round trip. This backend therefore:
 *  - fetches every chunk a read touches at once, not one after another;
 *  - reads ahead of sequential access;
 *  - retries failed or stalled requests instead of failing the guest read;
 *  - keeps fetched chunks in the origin-private file system, so a browser
 *    downloads each byte once, not on every launch;
 *  - fills that cache with the rest of the installation in the background,
 *    only while no guest read is waiting, so play becomes local over time.
 * The manifest names every file and size; the cache is keyed by its digest,
 * so a re-staged installation never reads stale chunks.
 */
addToLibrary({
  $recompStream: {
    chunk: 1024 * 1024,      // network and cache granularity
    memoryLimit: 192 * 1024 * 1024,
    readAhead: 6,            // chunks fetched past a sequential read
    siblingSize: 4 * 1024 * 1024,   // small files fetched with the first one opened in their folder
    siblingBudget: 32 * 1024 * 1024,
    demandSlots: 8,          // concurrent requests for waiting reads
    backgroundSlots: 3,      // concurrent requests for the background fill
    timeoutMs: 45000,        // a request that delivers nothing is retried
    attempts: 6,
  },
  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends', '$recompStream',
    '_wasmfs_fetch_get_file_url', '_wasmfs_fetch_get_chunk_size', 'recomp_web_stream_size',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    const cfg = recompStream;
    const memory = new Map();   // key -> Uint8Array, least recently used first
    let memoryBytes = 0;
    const pending = new Map();  // key -> {promise, priority}
    const sizes = new Map();    // url -> size, for files the manifest does not size
    const queue = [[], [], []]; // waiting network jobs by priority: demand, ahead, background
    const active = [0, 0, 0];
    let waitingReads = 0;
    const stats = { memory: 0, disk: 0, network: 0, networkBytes: 0, retries: 0, filled: 0, fillTotal: 0 };
    const channel = typeof BroadcastChannel == 'function' ? new BroadcastChannel('recomp-stream') : null;

    // ---- persistent cache --------------------------------------------------
    let disk = null;           // FileSystemDirectoryHandle for this manifest's chunks
    const onDisk = new Set();  // chunk names known to be complete on disk
    let diskWritable = true;
    let manifest = null;       // {base, text, files: [[path, size], ...]}
    let ready = null;

    function chunkName(path, index) {
      return encodeURIComponent(path) + '@' + index;
    }
    async function openCache(text, base) {
      try {
        const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(base + '\n' + text)));
        const key = Array.from(digest.subarray(0, 12), b => b.toString(16).padStart(2, '0')).join('');
        const root = await (await navigator.storage.getDirectory()).getDirectoryHandle('stream-cache', { create: true });
        for await (const name of root.keys())
          if (name !== key) await root.removeEntry(name, { recursive: true }).catch(() => {});
        disk = await root.getDirectoryHandle(key, { create: true });
        for await (const [name, handle] of disk.entries())
          if (name === 'base.txt') continue;
          else if (handle.kind === 'file' && !name.endsWith('.part')) onDisk.add(name);
          else await disk.removeEntry(name).catch(() => {});
        // Which installation this cache holds, so the launcher can delete it
        // with that game's data.
        const marker = await (await disk.getFileHandle('base.txt', { create: true })).createWritable();
        await marker.write(base);
        await marker.close();
      } catch (error) {
        disk = null;
        console.warn('[stream] persistent cache unavailable:', error.message || error);
      }
    }
    // Disk reads share one browser queue, so a read of a hundred chunks would
    // hold a one-chunk read behind it for hundreds of milliseconds - while the
    // guest thread that asked holds the scheduler and the game stands still.
    // A few at a time, the small reads first.
    const diskQueue = [[], [], [], []];  // small demand, bulk demand, read-ahead, background
    let diskActive = 0;
    const handles = new Map();
    function diskPump() {
      while (diskActive < 4) {
        const level = diskQueue.findIndex(q => q.length);
        if (level < 0) return;
        const job = diskQueue[level].shift();
        diskActive++;
        job.run().then(job.resolve, job.reject).finally(() => { diskActive--; diskPump(); });
      }
    }
    function diskRead(name, length, level) {
      if (!disk || !onDisk.has(name)) return Promise.resolve(null);
      return new Promise((resolve, reject) => {
        diskQueue[level].push({ resolve, reject, run: async () => {
          try {
            let handle = handles.get(name);
            if (!handle) handles.set(name, handle = await disk.getFileHandle(name));
            const file = await handle.getFile();
            if (file.size === length) return new Uint8Array(await file.arrayBuffer());
          } catch {}
          handles.delete(name);
          onDisk.delete(name);
          return null;
        } });
        diskPump();
      });
    }
    async function diskWrite(name, bytes) {
      if (!disk || !diskWritable || onDisk.has(name)) return;
      try {
        // Write under a temporary name and rename, so an interrupted write
        // never leaves a short chunk under the real name.
        const part = await disk.getFileHandle(name + '.part', { create: true });
        const out = await part.createWritable();
        await out.write(bytes);
        await out.close();
        await part.move(name);
        onDisk.add(name);
        stats.filled += bytes.length;
      } catch (error) {
        if (error && error.name === 'QuotaExceededError') {
          diskWritable = false;
          console.warn('[stream] browser storage is full; further data streams without caching');
        }
        if (disk) disk.removeEntry(name + '.part').catch(() => {});
      }
    }

    // ---- memory cache --------------------------------------------------------
    function remembered(key) {
      const bytes = memory.get(key);
      if (bytes) { memory.delete(key); memory.set(key, bytes); }
      return bytes;
    }
    function remember(key, bytes) {
      if (memory.has(key) || bytes.length > cfg.memoryLimit) return bytes;
      while (memoryBytes + bytes.length > cfg.memoryLimit && memory.size) {
        const oldest = memory.keys().next().value;
        memoryBytes -= memory.get(oldest).length;
        memory.delete(oldest);
      }
      cfg.memoryBytes = memoryBytes;
      memory.set(key, bytes);
      memoryBytes += bytes.length;
      cfg.memoryBytes = memoryBytes;  // for tests
      return bytes;
    }

    // ---- network ---------------------------------------------------------------
    const running = new Set();
    function start(p) {
      const job = queue[p].shift();
      active[p]++;
      job.controller = new AbortController();
      running.add(job);
      job.run(job.controller.signal).then(job.resolve, job.reject)
        .finally(() => { active[p]--; running.delete(job); pump(); });
    }
    function pump() {
      // A read the guest waits on gets the whole connection: background
      // downloads in flight are cancelled and fetched again later.
      if (waitingReads || queue[0].length)
        for (const job of running)
          if (job.entry.priority === 2) job.controller.abort();
      for (let p = 0; p < 2; ++p)
        while (queue[p].length && active[0] + active[1] < cfg.demandSlots) start(p);
      // The background fill never competes with a read the guest waits on.
      if (!waitingReads && !queue[0].length && !queue[1].length && !active[0] && !active[1])
        while (queue[2].length && active[2] < cfg.backgroundSlots) start(2);
    }
    function promote(key, priority) {
      for (let p = priority + 1; p < 3; ++p) {
        const at = queue[p].findIndex(job => job.key === key);
        if (at >= 0) { const [job] = queue[p].splice(at, 1); queue[priority].push(job); pump(); return; }
      }
    }
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
    async function download(url, start, end, size, cancel) {
      const whole = start === 0 && end === size;
      for (let attempt = 1; ; ++attempt) {
        const abort = new AbortController();
        const timer = setTimeout(() => abort.abort(), cfg.timeoutMs);
        const stop = () => abort.abort();
        if (cancel) cancel.addEventListener('abort', stop);
        try {
          const response = await fetch(url, whole ? { signal: abort.signal }
            : { signal: abort.signal, headers: { Range: `bytes=${start}-${end - 1}` } });
          if (response.status === 404) throw Object.assign(new Error('missing'), { status: 404, final: true });
          let bytes;
          if (response.status === 206 && !whole) {
            const range = response.headers.get('Content-Range');
            if (range !== `bytes ${start}-${end - 1}/${size}`) throw new Error('invalid asset range');
            bytes = new Uint8Array(await response.arrayBuffer());
          } else if (response.status === 200) {
            // Large assets need byte ranges. Never hold an unbounded whole
            // response in memory when a server ignores the request.
            if (!whole && size > cfg.memoryLimit) {
              if (response.body) await response.body.cancel();
              throw Object.assign(new Error('large streamed assets require HTTP byte ranges'), { final: true });
            }
            bytes = new Uint8Array(await response.arrayBuffer());
            if (bytes.length !== size) throw new Error('truncated streamed asset');
            if (!whole) bytes = bytes.slice(start, end);
          } else {
            throw Object.assign(new Error('HTTP ' + response.status), { status: response.status });
          }
          if (bytes.length !== end - start) throw new Error('truncated asset range');
          stats.network++;
          stats.networkBytes += bytes.length;
          return bytes;
        } catch (error) {
          if (cancel && cancel.aborted) throw Object.assign(new Error('preempted'), { preempted: true });
          if (error.final || attempt >= cfg.attempts) throw error;
          stats.retries++;
          console.warn(`[stream] retrying ${url.slice(url.lastIndexOf('/data/') + 6)} ${start}-${end} (${error.message || error.name}), attempt ${attempt + 1}`);
          await sleep(Math.min(8000, 500 * 2 ** (attempt - 1)));
        } finally {
          clearTimeout(timer);
          if (cancel) cancel.removeEventListener('abort', stop);
        }
      }
    }

    // ---- chunks ----------------------------------------------------------------
    function span(size) { return size <= 2 * cfg.chunk ? Math.max(size, 1) : cfg.chunk; }
    // One chunk of one file. Memory, then disk, then network; concurrent
    // requests for the same chunk share one promise.
    function chunk(info, index, priority, small = false) {
      const key = info.path + '@' + index;
      const have = remembered(key);
      if (have) { stats.memory++; return Promise.resolve(have); }
      const waiting = pending.get(key);
      if (waiting) {
        if (priority < waiting.priority) { waiting.priority = priority; promote(key, priority); }
        return waiting.promise;
      }
      const step = span(info.size);
      const start = index * step, end = Math.min(info.size, start + step);
      const name = chunkName(info.path, index);
      const entry = { priority };
      entry.promise = (async () => {
        const cached = await diskRead(name, end - start, priority === 0 ? (small ? 0 : 1) : priority + 1);
        if (cached) {
          stats.disk++;
          return priority === 2 ? cached : remember(key, cached);
        }
        const bytes = await new Promise((resolve, reject) => {
          const job = { key, entry, run: signal => download(info.url, start, end, info.size, signal), resolve, reject };
          // A one- or two-chunk read the guest waits on goes ahead of a bulk one.
          if (small && entry.priority === 0) queue[0].unshift(job);
          else queue[entry.priority].push(job);
          pump();
        });
        diskWrite(name, bytes);
        return entry.priority === 2 ? bytes : remember(key, bytes);
      })().finally(() => pending.delete(key));
      pending.set(key, entry);
      return entry.promise;
    }

    // ---- files -------------------------------------------------------------------
    const lastEnd = new Map();  // path -> end offset of its previous read
    function fileInfo(file) {
      const pointer = __wasmfs_fetch_get_file_url(file);
      const raw = UTF8ToString(pointer);
      // FetchDirectory joins a raw child path to the already encoded base URL
      // with two slashes. Escape child names (including # and %) only once.
      const seam = raw.indexOf('//', raw.indexOf('://') + 3);
      const path = seam < 0 ? raw : raw.slice(seam + 2);
      const url = seam < 0 ? raw : raw.slice(0, seam) + '/' + path.split('/').map(encodeURIComponent).join('/');
      return { url, path, base: seam < 0 ? raw : raw.slice(0, seam), size: _recomp_web_stream_size(pointer) };
    }
    // The manifest is read first, before the guest boots. Its size comes from
    // its body: a compressing proxy may strip Content-Length from HEAD.
    async function loadManifest(info) {
      if (manifest) return manifest;
      const response = await fetch(info.url, { cache: 'no-cache' });
      if (!response.ok) throw response;
      const text = await response.text();
      const files = [];
      for (const line of text.split('\n').slice(1)) {
        const tab = line.indexOf('\t');
        if (tab > 0) files.push([line.slice(tab + 1), Number(line.slice(0, tab))]);
      }
      manifest = { base: info.base, text, bytes: new TextEncoder().encode(text), files, folders: new Map() };
      for (const [path, size] of files) {
        const folder = path.slice(0, path.lastIndexOf('/') + 1);
        if (!manifest.folders.has(folder)) manifest.folders.set(folder, []);
        manifest.folders.get(folder).push([path, size]);
      }
      ready = openCache(text, info.base).then(() => { setTimeout(fill, 0); setInterval(report, 30000); });
      return manifest;
    }
    async function sizeOf(info) {
      if (info.size >= 0) return info.size;
      if (info.path === 'manifest.txt') return (await loadManifest(info)).bytes.length;
      if (!sizes.has(info.url)) {
        const response = await fetch(info.url, { method: 'HEAD' });
        if (!response.ok) throw response;
        const value = response.headers.get('Content-Length');
        const size = value === null ? NaN : Number(value);
        if (!Number.isSafeInteger(size) || size < 0) throw new Error('missing asset size');
        sizes.set(info.url, size);
      }
      return sizes.get(info.url);
    }

    // A game loading a level opens one small file after another in the same
    // folder; over the internet each would cost a round trip. The first read
    // in a folder fetches its other small files alongside, in parallel.
    const openedFolders = new Set();
    function prefetchFolder(path) {
      const folder = path.slice(0, path.lastIndexOf('/') + 1);
      if (!folder || !manifest || openedFolders.has(folder)) return;
      openedFolders.add(folder);
      let budget = cfg.siblingBudget;
      for (const [sibling, size] of manifest.folders.get(folder) || []) {
        if (sibling === path || size > cfg.siblingSize || size > budget) continue;
        budget -= size;
        const url = manifest.base + '/' + sibling.split('/').map(encodeURIComponent).join('/');
        const info = { url, path: sibling, size };
        const step = span(size);
        for (let index = 0; index * step < Math.max(size, 1); ++index) chunk(info, index, 1).catch(() => {});
      }
    }

    // ---- background fill ----------------------------------------------------
    // No game knowledge here: files the game has read come first (a track it
    // streams from is then completed early), then everything else from the
    // smallest up, so large movies and music arrive last.
    const touched = new Set();
    async function fill() {
      if (!disk || !manifest) return;
      try {
        const estimate = navigator.storage.estimate ? await navigator.storage.estimate() : null;
        const total = manifest.files.reduce((sum, [, size]) => sum + size, 0);
        if (estimate && estimate.quota - estimate.usage < total * 1.05) {
          console.warn('[stream] not enough browser storage to keep the whole installation; streaming on demand only');
          return;
        }
      } catch {}
      const files = new Map();
      let filled = 0, total = 0;
      for (const [path, size] of manifest.files) {
        const step = span(size), count = Math.max(1, Math.ceil(size / step));
        files.set(path, { path, size, count, cursor: 0 });
        for (let index = 0; index < count; ++index) {
          const bytes = Math.min(size, (index + 1) * step) - index * step;
          total += bytes;
          if (onDisk.has(chunkName(path, index))) filled += bytes;
        }
      }
      // Later disk writes, from play or from this fill, add to the count.
      stats.filled = filled;
      stats.fillTotal = total;
      report();
      const bySize = [...files.values()].sort((a, b) => a.size - b.size || a.path.localeCompare(b.path));
      let next = 0;
      const missing = file => {
        while (file.cursor < file.count) {
          const index = file.cursor++;
          if (!onDisk.has(chunkName(file.path, index))) return index;
        }
        return -1;
      };
      const later = [];  // chunks whose download a guest read preempted
      const pick = () => {
        if (later.length) return later.shift();
        for (const path of touched) {
          const file = files.get(path);
          const index = file ? missing(file) : -1;
          if (index >= 0) return [file, index];
        }
        for (; next < bySize.length; ++next) {
          const index = missing(bySize[next]);
          if (index >= 0) return [bySize[next], index];
        }
        return null;
      };
      const lane = async () => {
        for (let work; diskWritable && (work = pick());) {
          const [file, index] = work;
          const url = manifest.base + '/' + file.path.split('/').map(encodeURIComponent).join('/');
          try {
            await chunk({ url, path: file.path, size: file.size }, index, 2);
          } catch (error) {
            if (error.preempted) { later.push(work); continue; }
            console.warn('[stream] background download skipped', file.path, error.message || error.status);
          }
        }
      };
      await Promise.all(Array.from({ length: cfg.backgroundSlots }, lane));
      report();
    }
    // While the game waits on the network, tell the page so a long first
    // load reads as a download, not a hang.
    let lastBytes = 0, waitingSince = 0;
    setInterval(() => {
      const rate = stats.networkBytes - lastBytes;
      lastBytes = stats.networkBytes;
      if (!waitingReads) { if (waitingSince && channel) channel.postMessage({ waiting: false }); waitingSince = 0; return; }
      waitingSince = waitingSince || performance.now();
      if (channel && performance.now() - waitingSince > 1500) channel.postMessage({ waiting: true, rate });
    }, 1000);
    function report() {
      const message = {
        filled: stats.filled, total: stats.fillTotal, network: stats.networkBytes,
        cached: !!disk, retries: stats.retries,
      };
      if (channel) channel.postMessage(message);
      console.log(`[stream] ${(stats.filled / 1048576).toFixed(0)}/${(stats.fillTotal / 1048576).toFixed(0)} MiB kept locally; ` +
                  `hits memory ${stats.memory}, disk ${stats.disk}, network ${stats.network} ` +
                  `(${(stats.networkBytes / 1048576).toFixed(0)} MiB), retries ${stats.retries}`);
    }

    wasmFS$backends[backend] = {
      allocFile: async () => {},
      freeFile: async file => { sizes.delete(fileInfo(file).url); },
      write: async () => -{{{ cDefs.EROFS }}},
      read: async (file, buffer, length, offset) => {
        if (offset < 0 || length <= 0) return 0;
        let info;
        try {
          info = fileInfo(file);
          if (info.path === 'manifest.txt') {
            const bytes = (await loadManifest(info)).bytes;
            const count = Math.max(0, Math.min(length, bytes.length - offset));
            HEAPU8.set(bytes.subarray(offset, offset + count), buffer);
            return count;
          }
          if (ready) await ready;
          info.size = await sizeOf(info);
          length = Math.min(length, info.size - offset);
          if (length <= 0) return 0;
          const step = span(info.size);
          const end = offset + length;
          const first = Math.floor(offset / step), last = Math.floor((end - 1) / step);
          touched.add(info.path);
          prefetchFolder(info.path);
          const sequential = lastEnd.get(info.path) === offset;
          lastEnd.set(info.path, end);
          waitingReads++;
          const began = performance.now();
          let parts;
          try {
            const wanted = [];
            for (let index = first; index <= last; ++index) wanted.push(chunk(info, index, 0, last - first < 2));
            // Read ahead past the end of this read; further for streaming files.
            const count = Math.ceil(info.size / step);
            for (let index = last + 1; index <= Math.min(count - 1, last + (sequential ? cfg.readAhead : 2)); ++index)
              chunk(info, index, 1).catch(() => {});
            parts = await Promise.all(wanted);
          } finally {
            waitingReads--;
            pump();
          }
          // A read the guest waited on for long is a visible stall: name it.
          const waited = performance.now() - began;
          if (waited > 1000)
            console.warn(`[stream] the game waited ${(waited / 1000).toFixed(1)} s for ${info.path} ` +
                         `${offset}+${length} (${last - first + 1} chunk${last > first ? 's' : ''})`);
          for (let index = first, at = offset; index <= last; ++index) {
            const start = index * step;
            const bytes = parts[index - first];
            const count = Math.min(end, start + bytes.length) - at;
            HEAPU8.set(bytes.subarray(at - start, at - start + count), buffer + at - offset);
            at += count;
          }
          return length;
        } catch (error) {
          console.error('[stream] asset read failed:', info ? info.url : '?', error.message || error.status);
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
