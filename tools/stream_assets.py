"""Stage an explicitly selected installation for the read-only web fetch mount."""

import hashlib
from pathlib import Path, PurePosixPath
import shutil
import tempfile

import stage_game_files

HEADER = "recomp-stream-v1"
MANIFEST = "manifest.txt"
MARKER = ".recomp-stream"


def relative_path(name):
    """The manifest uses literal UTF-8 paths, one size/tab/path record per line."""
    if (not name or name.startswith("/") or any(c in name for c in "\\:")
            or any(ord(c) < 32 for c in name) or len(name.encode("utf-8")) > 1024
            or any(p in ("", ".", "..") for p in name.split("/"))):
        raise ValueError("Invalid streamed asset path: %r" % name)
    return name


def read_manifest(text):
    lines = text.splitlines()
    if not lines or lines[0] != HEADER:
        raise ValueError("Unsupported streamed asset manifest")
    files = {}
    folded = set()
    for line in lines[1:]:
        size, sep, name = line.partition("\t")
        relative_path(name)
        if not sep or not size.isascii() or not size.isdecimal() or int(size) > 2**53 - 1:
            raise ValueError("Invalid streamed asset size")
        if name.casefold() in folded or name.casefold() in (MANIFEST, MARKER):
            raise ValueError("Duplicate/reserved streamed asset path: %s" % name)
        folded.add(name.casefold())
        files[name] = int(size)
    return files


def digest(path):
    sha = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def stage(cfg, dest):
    """Publish a manifest only after copying verified, filtered private inputs.

    Only this generated data directory is managed. Stale files from its old
    manifest are removed individually; profiles and the source are never edited.
    """
    source, dest = cfg["developer_game_dir_path"].resolve(), Path(dest).resolve()
    if dest == source or source in dest.parents or dest in source.parents:
        raise ValueError("Stream output and game installation must be separate")
    if digest(cfg["developer_exe_path"]) != cfg["game"]["sha256"]:
        raise ValueError("Streaming requires the pinned executable; run setup first")
    for directory in cfg["setup"]["required_dirs"]:
        if not (source / directory).is_dir():
            raise ValueError("Missing required game directory: %s" % directory)
    inputs = {}
    patterns = [p.lower() for p in cfg["bundle"]["exclude"]]
    keep = stage_game_files.kept(cfg)
    for path in sorted(source.rglob("*")):
        name = path.relative_to(source).as_posix()
        if stage_game_files.excluded(PurePosixPath(name.lower()), patterns, keep):
            continue
        if path.is_file():
            if source not in path.resolve().parents:
                raise ValueError("Asset leaves installation: %s" % path)
            relative_path(name)
            key = name.casefold()
            if key in inputs:
                raise ValueError("Case-ambiguous asset: %s" % name)
            inputs[key] = (name, path)
    # Use each module's prepared image, which can differ from the installed DLL.
    for module in cfg.get("aux_modules", []):
        name = relative_path(module["name"])
        if digest(module["path"]) != module["sha256"]:
            raise ValueError("Prepared auxiliary module has the wrong hash: %s" % name)
        inputs[name.casefold()] = (name, module["path"])
    for name, _ in inputs.values():
        if name.casefold() in (MANIFEST, MARKER):
            raise ValueError("Reserved asset name: %s" % name)
        if any("/".join(name.split("/")[:i]).casefold() in inputs
               for i in range(1, len(name.split("/")))):
            raise ValueError("Asset collides with a directory: %s" % name)
    exe = "/".join(filter(None, (cfg["game"]["exe_dir"], cfg["game"]["executable"])))
    if exe.casefold() not in inputs:
        raise ValueError("Bundle exclusions remove the executable")
    if dest.exists() and any(dest.iterdir()) and not (dest / MARKER).is_file():
        raise ValueError("Refusing to replace an unmanaged data directory: %s" % dest)
    dest.mkdir(parents=True, exist_ok=True)
    (dest / MARKER).write_text(HEADER + "\n", encoding="utf-8")
    previous = read_manifest((dest / MANIFEST).read_text(encoding="utf-8")) if (dest / MANIFEST).is_file() else {}
    files = {}
    for name, path in sorted(inputs.values()):
        target = dest / name
        if dest not in target.resolve().parents:
            raise ValueError("Output path leaves data directory: %s" % target)
        stat = path.stat()
        if (not target.is_file() or target.stat().st_size != stat.st_size
                or target.stat().st_mtime_ns != stat.st_mtime_ns):
            target.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as tmp:
                temporary = Path(tmp.name)
            try:
                shutil.copy2(path, temporary)
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
        files[name] = stat.st_size
    current_names = {name.casefold() for name in files}
    for name in previous:
        if name.casefold() in current_names:
            continue
        target = dest / name
        if dest not in target.resolve().parents:
            raise ValueError("Stale output leaves data directory")
        target.unlink(missing_ok=True)
    text = HEADER + "\n" + "".join("%d\t%s\n" % (size, name) for name, size in files.items())
    temporary = dest / (MANIFEST + ".new")
    with temporary.open("w", encoding="utf-8", newline="\n") as file:
        file.write(text)
    temporary.replace(dest / MANIFEST)
    return files
