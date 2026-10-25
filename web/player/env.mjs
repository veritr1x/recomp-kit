// No environment switches can be changed by a URL unless a port explicitly
// adds an exact name here. In particular, URLs cannot replace profile paths
// or select native libraries. The default allowlist is empty.
export const QUERY_SWITCH_ALLOWLIST = Object.freeze([]);

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
