import { queryEnvironment, executablePath } from "./env.mjs";
import { selectSource } from "./source.mjs";
"use strict";
const statusEl = document.getElementById("status");
const canvas = document.getElementById("canvas");
function status(text, back) {
  statusEl.textContent = text;
  if (back) {
    const a = document.createElement("a");
    a.href = "../";
    a.textContent = "\nBack to the launcher";
    statusEl.append(document.createElement("br"), a);
  }
  statusEl.hidden = !text;
}
const log = [];
window.recompLog = log; // for tests
function record(text, warning) {
  log.push(text);
  if (log.length > 2000) log.shift();
  if (text.startsWith("[stream] failed:"))
    status("The hosted game files could not be loaded. Return to the launcher and retry or import your copy.", true);
  (warning ? console.warn : console.log)(text);
}

async function gameId() {
  const parts = location.pathname.split("/").filter(Boolean);
  if (parts.length && parts[parts.length - 1].includes(".")) parts.pop();
  return decodeURIComponent(parts[parts.length - 1] || "");
}

async function start() {
  if (!crossOriginIsolated) throw new Error("This page must be served cross-origin isolated (COOP/COEP headers).");
  if (!navigator.gpu) throw new Error("This browser has no WebGPU.");
  const id = await gameId();
  const gamesUrl = new URL("../games.json", location.href).href;
  const games = await fetch(gamesUrl).then((r) => r.json());
  const game = games.find((g) => g.id === id);
  if (!game) throw new Error(`No game "${id}" in the launcher's games.json.`);
  document.title = game.title;

  // Imported data remains preferred by default. Explicit streaming never
  // modifies that installation, and both modes share the same OPFS profile.
  let imported = false;
  let dir = await navigator.storage.getDirectory();
  try {
    for (const p of [id, "game"]) dir = await dir.getDirectoryHandle(p);
    const components = executablePath(game).split("/");
    const file = components.pop();
    for (const part of components) dir = await dir.getDirectoryHandle(part);
    await dir.getFileHandle(file);
    imported = (await (await dirForStamp(id)).text()).trim() === game.sha256;
  } catch {}
  const source = await selectSource(game, gamesUrl, location.search, imported);
  window.recompSource = source.kind;

  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("WebGPU gave no adapter.");
  const wanted = ["texture-compression-bc", "depth32float-stencil8", "float32-filterable",
                  "unorm16-texture-formats", "rg11b10ufloat-renderable"];
  const requiredFeatures = wanted.filter((f) => adapter.features.has(f));
  const requiredLimits = {};
  for (const k of ["maxTextureDimension2D", "maxBufferSize", "maxStorageBufferBindingSize",
                   "maxUniformBufferBindingSize", "maxBindGroups", "maxSampledTexturesPerShaderStage",
                   "maxSamplersPerShaderStage", "maxStorageBuffersPerShaderStage",
                   "maxUniformBuffersPerShaderStage", "maxDynamicUniformBuffersPerPipelineLayout",
                   "maxVertexBuffers", "maxVertexAttributes", "maxColorAttachments",
                   "maxInterStageShaderVariables"])
    if (k in adapter.limits) requiredLimits[k] = adapter.limits[k];
  const device = await adapter.requestDevice({ requiredFeatures, requiredLimits });
  device.lost.then((info) => status(`The graphics device was lost: ${info.message}`, true));
  device.addEventListener("uncapturederror", (e) => console.error("WebGPU:", e.error.message));

  const Module = {
    canvas,
    preinitializedWebGPUDevice: device,
    noInitialRun: true,
    print: (t) => record(t, false),
    printErr: (t) => record(t, true),
    setStatus: (t) => status(t ? t : ""),
    onAbort: (what) => status(`The game stopped: ${what}`, true),
    preRun: [() => {
      const m = Module;
      m.ENV.RECOMP_PROFILE_DIR = `/opfs/${id}/profile`;
      Object.assign(m.ENV, queryEnvironment(location.search));
    }],
    onRuntimeInitialized() {
      status("");
      canvas.focus();
      Module.callMain(source.args);
    },
  };
  window.Module = Module;
  const script = document.createElement("script");
  script.src = "@APP@.js";
  script.onerror = () => status("The game's web build is missing.", true);
  document.body.append(script);
}

async function dirForStamp(id) {
  const root = await navigator.storage.getDirectory();
  const game = await (await root.getDirectoryHandle(id)).getDirectoryHandle("game");
  return (await game.getFileHandle(".stamp")).getFile();
}

canvas.addEventListener("contextmenu", (e) => e.preventDefault());
canvas.addEventListener("click", () => canvas.focus());
start().catch((e) => status(e.message, true));
