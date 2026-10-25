import { queryEnvironment, executablePath } from "./env.mjs";
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

async function gameId() {
  const parts = location.pathname.split("/").filter(Boolean);
  if (parts.length && parts[parts.length - 1].includes(".")) parts.pop();
  return decodeURIComponent(parts[parts.length - 1] || "");
}

async function start() {
  if (!crossOriginIsolated) throw new Error("This page must be served cross-origin isolated (COOP/COEP headers).");
  if (!navigator.gpu) throw new Error("This browser has no WebGPU.");
  const id = await gameId();
  const games = await fetch("../games.json").then((r) => r.json());
  const game = games.find((g) => g.id === id);
  if (!game) throw new Error(`No game "${id}" in the launcher's games.json.`);
  document.title = game.title;

  // The game must have been imported by the launcher first.
  let dir = await navigator.storage.getDirectory();
  try {
    for (const p of [id, "game"]) dir = await dir.getDirectoryHandle(p);
    const components = executablePath(game).split("/");
    const file = components.pop();
    for (const part of components) dir = await dir.getDirectoryHandle(part);
    await dir.getFileHandle(file);
  } catch {
    throw new Error(`${game.title} has not been imported yet.`);
  }

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

  const exe = `/opfs/${id}/game/${executablePath(game)}`;
  const Module = {
    canvas,
    preinitializedWebGPUDevice: device,
    noInitialRun: true,
    print: (t) => { log.push(t); console.log(t); },
    printErr: (t) => { log.push(t); console.warn(t); },
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
      Module.callMain([`--exe=${exe}`]);
    },
  };
  window.Module = Module;
  const script = document.createElement("script");
  script.src = "@APP@.js";
  script.onerror = () => status("The game's web build is missing.", true);
  document.body.append(script);
}

canvas.addEventListener("contextmenu", (e) => e.preventDefault());
canvas.addEventListener("click", () => canvas.focus());
start().catch((e) => status(e.message, true));
