#!/usr/bin/env python3
"""Build the web launcher: the hub page for one or more games.

    tools/web_launcher.py --game-dir /abs/game-a [--game-dir /abs/game-b ...] --out <dir>
        [--web-build <game id>=<build dir> ...] [--serve [port]] [--host ADDRESS]

Writes <dir>/index.html, app.js, core.js, worker.js and games.json (each game's
title, executable, digest, required folders, exclusions and store link from its
game.toml). A game's web build, when one exists, goes in <dir>/<game id>/.
--web-build copies a game's web build (build/web/recomp: index.html and the
app's .js/.wasm/.data) there. Serve <dir> over HTTPS or from localhost with
COOP/COEP headers (the game needs a secure, cross-origin isolated context);
--serve runs such a server for local testing."""

import argparse
import functools
import http.server
import json
import os
from pathlib import Path
import re
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import game_config  # noqa: E402
import stream_assets  # noqa: E402

PAGE_FILES = ("index.html", "style.css", "app.js", "core.js", "worker.js")
CSP = "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; connect-src 'self'; object-src 'none'; base-uri 'none'"


def game_entry(cfg):
    game, launcher = cfg["game"], cfg["launcher"]
    return {
        "id": game["id"],
        "title": launcher["title"],
        "executable": game["executable"],
        "exeDir": game["exe_dir"],
        "sha256": game["sha256"],
        "requiredDirs": cfg["setup"]["required_dirs"],
        "exclude": cfg["bundle"]["exclude"],
        "store": launcher["store"],
        "installNames": launcher["install_names"],
        "minFreeMb": launcher["min_free_mb"],
    }


def build(game_dirs, out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    games = [game_entry(game_config.load(d)) for d in game_dirs]
    ids = [g["id"] for g in games]
    if len(set(ids)) != len(ids):
        raise ValueError("two games share an id: %s" % ", ".join(ids))
    for name in PAGE_FILES:
        shutil.copy2(ROOT / "web/launcher" / name, out / name)
    (out / "games.json").write_text(json.dumps(games, indent=2) + "\n")
    return games


def stage_streams(game_dirs, games, ids, out):
    """Only explicit --stream selections publish private game data."""
    selections = {}
    for spec in ids:
        game_id, sep, install = spec.partition("=")
        if game_id in selections or (sep and not install):
            raise ValueError("--stream wants one ID or ID=INSTALL per game")
        selections[game_id] = Path(install).expanduser().resolve() if sep else None
    known = {g["id"] for g in games}
    if set(selections) - known:
        raise ValueError("--stream wants a known game id: %s" % ", ".join(sorted(known)))
    for game_dir, game in zip(game_dirs, games):
        if game["id"] in selections:
            cfg = game_config.load(game_dir)
            install = selections[game["id"]]
            if install is not None:
                # Analysis can use a minimal private executable copy. Hosting
                # needs the full installation; preserve the profile's prepared
                # auxiliary module overrides and all pinned image checks.
                cfg = dict(cfg, developer_game_dir_path=install,
                           developer_exe_path=install / game["exeDir"] / game["executable"])
            files = stream_assets.stage(cfg, Path(out) / game["id"] / "data")
            game["streamManifest"] = "./%s/data/manifest.txt" % game["id"]
            print("staged %s for streaming: %d files, %d bytes" %
                  (game["title"], len(files), sum(files.values())), flush=True)
    (Path(out) / "games.json").write_text(json.dumps(games, indent=2) + "\n", encoding="utf-8")


WEB_BUILD_SUFFIXES = (".js", ".mjs", ".css", ".wasm", ".data")


def copy_web_build(game_id, build, out):
    build, dest = Path(build), Path(out) / game_id
    if not (build / "index.html").is_file():
        raise ValueError("no web build in %s (index.html is missing)" % build)
    dest.mkdir(parents=True, exist_ok=True)
    for f in build.iterdir():
        if f.is_file() and (f.name in ("index.html", "FETCH-NOTICE.txt") or f.suffix in WEB_BUILD_SUFFIXES):
            shutil.copy2(f, dest / f.name)


def is_stream_data(url_path):
    """A staged installation file: <game id>/data/<path>."""
    parts = url_path.split("?", 1)[0].split("/")
    return len(parts) > 3 and parts[2] == "data"


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript", ".data": "application/octet-stream"}
    # Keep-alive: over the internet, a new connection per range costs a round trip.
    protocol_version = "HTTP/1.1"

    def guess_type(self, path):
        # Streamed game files are opaque bytes. A text type invites a proxy to
        # compress the manifest, which removes the Content-Length it is sized by.
        if is_stream_data(self.path):
            return "application/octet-stream"
        return super().guess_type(path)

    def send_head(self):
        """Serve one byte range; HEAD always describes the complete file."""
        self.bytes_left = None
        path = Path(self.translate_path(self.path))
        root = Path(self.directory).resolve()
        if path.resolve() != root and root not in path.resolve().parents:
            self.send_error(403, "Path leaves site")
            return None
        if not path.is_file():
            return super().send_head()
        file = path.open("rb")
        size = path.stat().st_size
        start, end = 0, size - 1
        requested = self.headers.get("Range") if self.command == "GET" else None
        if requested:
            match = re.fullmatch(r"bytes=(\d*)-(\d*)", requested)
            if match and any(match.groups()):
                left, right = match.groups()
                if left:
                    start = int(left)
                    end = min(int(right), end) if right else end
                elif int(right):
                    start = max(size - int(right), 0)
                else:
                    start = size
            else:
                start = size
            if start > end or start >= size:
                file.close()
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return None
        self.send_response(206 if requested else 200)
        self.send_header("Content-Type", self.guess_type(str(path)))
        self.send_header("Content-Length", str(max(0, end - start + 1)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Last-Modified", self.date_time_string(path.stat().st_mtime))
        if requested:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        file.seek(start)
        self.bytes_left = max(0, end - start + 1)
        return file

    def copyfile(self, source, outputfile):
        if self.bytes_left is None:
            return super().copyfile(source, outputfile)
        try:
            outputfile.flush()
            if hasattr(os, "sendfile") and self.bytes_left:
                self.connection.sendfile(source, source.tell(), self.bytes_left)
                return
            while self.bytes_left:
                block = source.read(min(1024 * 1024, self.bytes_left))
                if not block:
                    break
                outputfile.write(block)
                self.bytes_left -= len(block)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Content-Security-Policy", CSP)
        # The player keeps streamed chunks in the browser's private storage;
        # an HTTP cache copy would only double that. no-transform keeps a
        # proxy from re-encoding the bytes it is asked for by range.
        self.send_header("Cache-Control", "no-store, no-transform" if is_stream_data(self.path) else "no-cache")
        super().end_headers()


def serve(out, port, host="127.0.0.1"):
    handler = functools.partial(IsolatedHandler, directory=str(out))
    with http.server.ThreadingHTTPServer((host, port), handler) as httpd:
        shown = "localhost" if host in ("127.0.0.1", "::1", "localhost") else host
        print("serving %s at http://%s:%d/" % (out, shown, port), flush=True)
        if shown != "localhost":
            print("note: browsers require HTTPS for this page outside localhost; put a TLS proxy in front",
                  flush=True)
        httpd.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game-dir", type=Path, action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--web-build", action="append", default=[], metavar="ID=DIR")
    parser.add_argument("--serve", type=int, nargs="?", const=8000, metavar="PORT")
    parser.add_argument("--host", default="127.0.0.1",
                        help="address --serve listens on (default loopback; 0.0.0.0 in a container)")
    parser.add_argument("--stream", action="append", default=[], metavar="ID[=INSTALL]",
                        help="stage a private installation (optionally select a full install directory)")
    args = parser.parse_args()
    games = build(args.game_dir, args.out)
    try:
        stage_streams(args.game_dir, games, args.stream, args.out)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    ids = {g["id"] for g in games}
    for spec in args.web_build:
        game_id, sep, build_dir = spec.partition("=")
        if not sep or game_id not in ids:
            parser.error("--web-build wants <game id>=<dir> for one of: %s" % ", ".join(sorted(ids)))
        copy_web_build(game_id, build_dir, args.out)
    print("web launcher for %s in %s" % (", ".join(g["title"] for g in games), args.out))
    if args.serve is not None:
        serve(args.out, args.serve, args.host)


if __name__ == "__main__":
    main()
