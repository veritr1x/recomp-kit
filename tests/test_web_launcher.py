"""tools/web_launcher.py writes the hub page and each game's launcher data."""

import importlib.util
import json
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading
import unittest
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("web_launcher", ROOT / "tools/web_launcher.py")
web_launcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(web_launcher)


class WebLauncherTests(unittest.TestCase):
    def test_hub_for_two_games(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            other = tmp / "other"
            other.mkdir()
            text = (ROOT / "games/stub/game.toml").read_text().replace('id = "stub"', 'id = "other"')
            (other / "game.toml").write_text(text + '\n[launcher]\ntitle = "Other"\nstore = "https://x.test"\n'
                                                    '\n[setup]\nrequired_dirs = ["data"]\n')
            shutil.copy(ROOT / "games/stub/globals.toml", other / "globals.toml")
            out = tmp / "site"
            games = web_launcher.build([ROOT / "games/stub", other], out)
            self.assertEqual([g["id"] for g in games], ["stub", "other"])
            for name in web_launcher.PAGE_FILES:
                self.assertTrue((out / name).is_file(), name)
            data = json.loads((out / "games.json").read_text())
            self.assertEqual(data[1]["title"], "Other")
            self.assertEqual(data[1]["requiredDirs"], ["data"])
            self.assertEqual(data[0]["executable"], "STUB.EXE")
            self.assertEqual(data[0]["sha256"], "0" * 64)
            web_launcher.stage_streams([ROOT / "games/stub", other], games, [], out)
            self.assertFalse(any("streamManifest" in g for g in json.loads((out / "games.json").read_text())))
            self.assertFalse((out / "stub/data").exists())
            with self.assertRaisesRegex(ValueError, "known game id"):
                web_launcher.stage_streams([ROOT / "games/stub", other], games, ["unknown"], out)
            with self.assertRaises(ValueError):
                web_launcher.build([ROOT / "games/stub", ROOT / "games/stub"], tmp / "dup")

    def test_web_build_copy(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            build = tmp / "build"
            build.mkdir()
            with self.assertRaises(ValueError):
                web_launcher.copy_web_build("stub", build, tmp / "site")
            for name in ("index.html", "player.js", "env.mjs", "source.mjs", "FETCH-NOTICE.txt", "style.css", "App.js", "App.wasm", "App.data", "libgen.a"):
                (build / name).write_text(name)
            web_launcher.copy_web_build("stub", build, tmp / "site")
            copied = sorted(f.name for f in (tmp / "site/stub").iterdir())
            self.assertEqual(copied, ["App.data", "App.js", "App.wasm", "FETCH-NOTICE.txt", "env.mjs", "index.html", "player.js", "source.mjs", "style.css"])
            # Each staging stamps the player's URLs with a digest of the build.
            (build / "player.js").write_text('import "./env.mjs?v=__RECOMP_BUILD__";')
            first = web_launcher.copy_web_build("stub", build, tmp / "site")
            self.assertEqual((tmp / "site/stub/player.js").read_text(), f'import "./env.mjs?v={first}";')
            self.assertEqual((tmp / "site/stub/App.wasm").read_text(), "App.wasm")
            (build / "App.wasm").write_text("App.wasm, rebuilt")
            self.assertNotEqual(web_launcher.copy_web_build("stub", build, tmp / "site"), first)

    def test_https_server(self):
        openssl = shutil.which("openssl")
        if not openssl:
            self.skipTest("openssl is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "index.html").write_text("hub")
            subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-keyout", str(tmp / "key.pem"), "-out", str(tmp / "cert.pem"),
                            "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1"],
                           check=True, capture_output=True)
            with web_launcher.make_server(tmp, 0, "127.0.0.1", tmp / "cert.pem", tmp / "key.pem") as httpd:
                thread = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
                thread.start()
                try:
                    context = ssl.create_default_context(cafile=str(tmp / "cert.pem"))
                    url = "https://127.0.0.1:%d/index.html" % httpd.server_port
                    with urllib.request.urlopen(url, context=context, timeout=10) as response:
                        self.assertEqual(response.read(), b"hub")
                        self.assertEqual(response.headers["Cross-Origin-Embedder-Policy"], "require-corp")
                        self.assertEqual(response.headers["Cross-Origin-Opener-Policy"], "same-origin")
                finally:
                    httpd.shutdown()
                    thread.join()
            with self.assertRaises(ssl.SSLError):
                web_launcher.make_server(tmp, 0, "127.0.0.1", tmp / "index.html")

    def test_core_suite(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")
        run = subprocess.run([node, "--test", *map(str, sorted((ROOT / "web/launcher/tests").glob("*.test.mjs")))],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
