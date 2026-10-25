"""Both web entry pages work without inline scripts, styles or event handlers."""
from html.parser import HTMLParser
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT / "tools"))
import web_launcher  # noqa: E402


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tags=[]

    def handle_starttag(self,tag,attrs):
        self.tags.append((tag,dict(attrs)))


def test_pages_enforce_strict_csp():
    for name in ("launcher","player"):
        path = ROOT / "web" / name / "index.html"
        page = Page()
        html = path.read_text(encoding="utf-8")
        page.feed(html)
        metas = [attrs for tag,attrs in page.tags if tag == "meta"
                 and attrs.get("http-equiv") == "Content-Security-Policy"]
        assert [meta["content"] for meta in metas] == [web_launcher.CSP]
        assert "'unsafe-inline'" not in web_launcher.CSP and "'unsafe-eval'" not in web_launcher.CSP
        for tag,attrs in page.tags:
            assert tag != "style"
            assert not any(key.startswith("on") or key == "style" for key in attrs)
            if tag == "script":
                assert attrs.get("src")
                assert (path.parent / attrs["src"]).is_file()
        assert not re.search(r"<script[^>]*>\s*[^<\s]",html)


def test_player_assets_are_staged_by_cmake():
    cmake = (ROOT / "host/CMakeLists.txt").read_text(encoding="utf-8")
    for asset in ("index.html","player.js","style.css","env.mjs"):
        assert f"web/player/{asset}" in cmake
