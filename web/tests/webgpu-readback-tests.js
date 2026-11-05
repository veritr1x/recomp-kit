"use strict";

async function start() {
  const status = document.getElementById("status");
  if (!crossOriginIsolated) throw new Error("Serve with COOP/COEP headers.");
  if (!navigator.gpu) throw new Error("WebGPU is unavailable.");
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
  if (!adapter) throw new Error("No WebGPU adapter.");
  const device = await adapter.requestDevice();
  device.addEventListener("uncapturederror", e => console.error("WebGPU:", e.error.message));
  window.Module = {
    preinitializedWebGPUDevice: device,
    noInitialRun: true,
    print: console.log,
    printErr: console.warn,
    onAbort(reason) { status.textContent = "Aborted: " + reason; },
    onRuntimeInitialized() {
      status.textContent = "Running texture, buffer, completion and device-loss checks…";
      Module.callMain([]);
    },
  };
  const script = document.createElement("script");
  script.src = "gpu_webgpu_readback_tests.js";
  script.onerror = () => { status.textContent = "Build gpu_webgpu_readback_tests first."; };
  document.body.append(script);
  const deadline = performance.now() + 30000;
  const timer = setInterval(() => {
    if (window.gpuReadbackFailures !== undefined) {
      status.textContent = `${window.gpuReadbackChecks} checks, ${window.gpuReadbackFailures} failures`;
      clearInterval(timer);
    } else if (performance.now() >= deadline) {
      status.textContent = "Timed out waiting for the worker.";
      clearInterval(timer);
    }
  }, 50);
}

start().catch(e => { document.getElementById("status").textContent = e.message; });
