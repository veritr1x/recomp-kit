import { executablePath } from "./env.mjs?v=__RECOMP_BUILD__";

// Both entry points resolve relative to games.json, so a site may live under
// a URL prefix. Runtime paths and profile locations never come from a URL.
export function streamManifestUrl(game, gamesUrl) {
  if (!game.streamManifest) return null;
  const url = new URL(game.streamManifest, gamesUrl);
  if (url.origin !== new URL(gamesUrl).origin || url.search || url.hash ||
      !url.pathname.endsWith("/data/manifest.txt"))
    throw new Error("Invalid hosted game manifest URL.");
  return url;
}

export async function selectSource(game, gamesUrl, search, imported, fetcher = fetch) {
  const wanted = new URLSearchParams(search).get("source");
  if (wanted && wanted !== "import" && wanted !== "stream")
    throw new Error("Unknown game source.");
  if (wanted !== "stream" && imported)
    return { kind: "import", args: [`--exe=/opfs/${game.id}/game/${executablePath(game)}`] };
  if (wanted !== "import") {
    const manifest = streamManifestUrl(game, gamesUrl);
    if (manifest) {
      const response = await fetcher(manifest.href, { method: "HEAD" });
      if (response.ok) {
        const base = new URL("./", manifest).href.replace(/\/$/, "");
        return { kind: "stream", args: [`--exe=/data/${executablePath(game)}`, "--stream-base", base] };
      }
      if (wanted === "stream" || response.status !== 404)
        throw new Error("The hosted game files are unavailable. Return to the launcher to import your copy.");
    }
  }
  throw new Error(`${game.title} has not been imported yet.`);
}
