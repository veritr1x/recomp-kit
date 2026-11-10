// No environment switches can be changed by a URL unless a port explicitly
// adds an exact name here. In particular, URLs cannot replace profile paths
// or select native libraries. The default allowlist is empty.
// Switches a shared link may set: rendering choices a tester can flip to
// narrow down a fault (RECOMP_D3D9_FFP=0 skips draws with no shaders bound;
// RECOMP_D3D9_FRAMES_AHEAD=0..4 sets how far Present runs ahead). Nothing here
// names a path, loads code or changes the game's files.
export const QUERY_SWITCH_ALLOWLIST = Object.freeze(["RECOMP_D3D9_FFP", "RECOMP_D3D9_FRAMES_AHEAD"]);

export function queryEnvironment(search, allowlist = QUERY_SWITCH_ALLOWLIST) {
  const allowed = new Set(allowlist);
  const result = Object.create(null);
  for (const [key, value] of new URLSearchParams(search))
    if (/^RECOMP_[A-Z0-9_]+$/.test(key) && allowed.has(key)) result[key] = value;
  return result;
}

export function executablePath(game) {
  return [game.exeDir, game.executable].filter(Boolean).join("/");
}
