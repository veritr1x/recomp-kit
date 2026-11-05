"""Private staging and real HTTP range semantics for the optional web mount."""
import functools
import hashlib
import http.client
import http.server
from pathlib import Path
import sys
import threading

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import stream_assets
import web_launcher


@pytest.fixture
def install(tmp_path):
    source = tmp_path / "install"
    (source / "data").mkdir(parents=True)
    (source / "Binaries/Win32").mkdir(parents=True)
    exe = source / "Binaries/Win32/Game.exe"
    exe.write_bytes(b"synthetic supported executable")
    (source / "data/space #%.bin").write_bytes(bytes(range(32)))
    (source / "skip").mkdir()
    (source / "skip/private.save").write_bytes(b"not for hosting")
    (source / "helper.dll").write_bytes(b"not loaded")
    return {
        "developer_game_dir_path": source, "developer_exe_path": exe,
        "game": {"id": "sample", "exe_dir": "Binaries/Win32", "executable": "Game.exe",
                 "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()},
        "bundle": {"exclude": ["SKIP", "*.DLL"]}, "setup": {"required_dirs": ["data"]},
        "aux_modules": [],
    }


def test_staging_hash_exclusions_auxiliary_override_and_stale_files(install, tmp_path):
    prepared = tmp_path / "prepared.dll"
    prepared.write_bytes(b"prepared module")
    (install["developer_game_dir_path"] / "Module.dll").write_bytes(b"installed module")
    install["aux_modules"] = [{"name": "Module.dll", "path": prepared,
                               "sha256": stream_assets.digest(prepared)}]
    out = tmp_path / "site/sample/data"
    files = stream_assets.stage(install, out)
    assert set(files) == {"Binaries/Win32/Game.exe", "data/space #%.bin", "Module.dll"}
    assert (out / "Module.dll").read_bytes() == b"prepared module"
    assert stream_assets.read_manifest((out / "manifest.txt").read_text()) == files
    before = (out / "data/space #%.bin").stat().st_mtime_ns
    assert stream_assets.stage(install, out) == files
    assert (out / "data/space #%.bin").stat().st_mtime_ns == before
    install["bundle"]["exclude"].append("*.bin")
    stream_assets.stage(install, out)
    assert not (out / "data/space #%.bin").exists()
    assert (install["developer_game_dir_path"] / "data/space #%.bin").is_file()


def test_wrong_hash_or_unmanaged_output_is_preserved(install, tmp_path):
    dest = tmp_path / "data"
    dest.mkdir()
    (dest / "keep.txt").write_text("keep")
    with pytest.raises(ValueError, match="unmanaged"):
        stream_assets.stage(install, dest)
    install["game"]["sha256"] = "0" * 64
    with pytest.raises(ValueError, match="pinned executable"):
        stream_assets.stage(install, tmp_path / "new")
    assert (dest / "keep.txt").read_text() == "keep"
    assert not (tmp_path / "new").exists()


def test_explicit_full_install_keeps_hashes_auxiliary_overrides_and_config(install, tmp_path, monkeypatch):
    prepared = tmp_path / "prepared.dll"
    prepared.write_bytes(b"prepared module")
    install["aux_modules"] = [{"name": "Module.dll", "path": prepared,
                               "sha256": stream_assets.digest(prepared)}]
    full = install["developer_game_dir_path"]
    cfg = dict(install, developer_game_dir_path=tmp_path / "analysis-only",
               developer_exe_path=tmp_path / "analysis-only/Game.exe")
    monkeypatch.setattr(web_launcher.game_config, "load", lambda _: cfg)
    game = {"id": "sample", "title": "Sample", "exeDir": "Binaries/Win32", "executable": "Game.exe"}
    web_launcher.stage_streams([tmp_path], [game], ["sample=" + str(full)], tmp_path / "site")
    assert (tmp_path / "site/sample/data/Module.dll").read_bytes() == prepared.read_bytes()
    assert game["streamManifest"] == "./sample/data/manifest.txt"
    assert cfg["developer_game_dir_path"] == tmp_path / "analysis-only"
    cfg["game"]["sha256"] = "0" * 64
    with pytest.raises(ValueError, match="pinned executable"):
        web_launcher.stage_streams([tmp_path], [game], ["sample=" + str(full)], tmp_path / "wrong")
    for selection in (["sample="], ["sample", "sample"], ["unknown=" + str(full)]):
        with pytest.raises(ValueError):
            web_launcher.stage_streams([tmp_path], [game], selection, tmp_path / "invalid")


def test_staging_cannot_write_into_or_over_its_input(install):
    root = install["developer_game_dir_path"]
    for dest in (root, root / "generated", root.parent):
        with pytest.raises(ValueError, match="separate"):
            stream_assets.stage(install, dest)


@pytest.mark.parametrize("name", ["../escape", "/absolute", "a//b", "a/./b", "a\\b", "C:x", "line\nname", "tab\tname"])
def test_manifest_rejects_ambiguous_paths(name):
    with pytest.raises(ValueError):
        stream_assets.relative_path(name)


def test_manifest_rejects_duplicate_and_invalid_sizes():
    for text in ("bad\n", "recomp-stream-v1\n1\tA\n2\ta\n",
                 "recomp-stream-v1\n-1\ta\n", "recomp-stream-v1\n1\tmanifest.txt\n"):
        with pytest.raises(ValueError):
            stream_assets.read_manifest(text)


@pytest.fixture
def server(tmp_path):
    (tmp_path / "data").mkdir()
    (tmp_path / "data/space #%.bin").write_bytes(bytes(range(32)))
    (tmp_path / "data/empty.bin").write_bytes(b"")
    handler = functools.partial(web_launcher.IsolatedHandler, directory=str(tmp_path))
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    yield httpd.server_port
    httpd.shutdown()
    httpd.server_close()
    thread.join()


def request(port, method="GET", path="/data/space%20%23%25.bin", headers=None):
    client = http.client.HTTPConnection("127.0.0.1", port)
    client.request(method, path, headers=headers or {})
    response = client.getresponse()
    result = response.status, dict(response.getheaders()), response.read()
    client.close()
    return result


@pytest.mark.parametrize("value,start,end", [("bytes=4-11", 4, 11), ("bytes=28-", 28, 31),
                                            ("bytes=-4", 28, 31), ("bytes=28-99", 28, 31)])
def test_real_range_responses(server, value, start, end):
    status, headers, body = request(server, headers={"Range": value})
    assert status == 206
    assert body == bytes(range(start, end + 1))
    assert headers["Content-Range"] == f"bytes {start}-{end}/32"
    assert headers["Content-Length"] == str(len(body))
    assert headers["Cross-Origin-Opener-Policy"] == "same-origin"
    assert headers["Cross-Origin-Embedder-Policy"] == "require-corp"


@pytest.mark.parametrize("value", ["bytes=99-", "bytes=9-4", "bytes=-0", "bytes=", "bytes=0-1,4-5", "garbage"])
def test_unsatisfiable_or_invalid_ranges(server, value):
    status, headers, body = request(server, headers={"Range": value})
    assert status == 416 and body == b""
    assert headers["Content-Range"] == "bytes */32"


def test_head_reports_complete_file_and_empty_files_work(server):
    status, headers, body = request(server, "HEAD", headers={"Range": "bytes=4-8"})
    assert status == 200 and body == b""
    assert headers["Content-Length"] == "32" and headers["Accept-Ranges"] == "bytes"
    assert request(server)[2] == bytes(range(32))
    assert request(server, path="/data/empty.bin")[0:3:2] == (200, b"")
    assert request(server, path="/missing")[0] == 404
