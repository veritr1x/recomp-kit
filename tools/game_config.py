"""Load a game directory: games/<id>/game.toml plus its curated globals file.

This module is the only place that knows the schema. Python 3.9 has no
tomllib, so the pinned tomli is the fallback."""

from pathlib import Path
import re

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib

REQUIRED_GAME_KEYS = ("id", "name", "app_name", "bundle_id", "executable", "sha256",
                      "image_base", "entry_point", "guest_root", "developer_exe")

HEAP_BASE_DEFAULT = 0x01000000

# The settings page's rows, in mods/display_settings.h DisplayRow order; the
# controls' seven rows are one entry. [settings] rows names the ones a game
# shows. Without the key a game shows every row. "keypad" is the pre-touch-
# controls spelling of "controls"; SETTINGS_ROW_ALIASES keeps it loading.
SETTINGS_ROWS = ("rendering", "ui_scale", "wide_view", "window", "resolution", "frame_limit",
                 "performance_overlay", "textures", "filtering", "controls")
SETTINGS_ROW_ALIASES = {"keypad": "controls"}

# [controls] default_layout and the on-screen layout picker's choices.
CONTROLS_LAYOUTS = ("pad", "keys", "pad+keys", "hidden")
# [controls.mapped] left_stick/right_stick/dpad modes.
STICK_MODES = ("cursor", "arrows", "horizontal_arrows", "wasd", "scroll", "wheel", "none")
# [controls.native] buttons: the pad button names a physical button maps to.
PAD_BUTTONS = ("cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
               "select", "start", "ps")
MAPPED_DEFAULTS = {"left_stick": "arrows", "right_stick": "cursor", "dpad": "arrows",
                   "cursor_speed": 900, "cross": "mouse_left", "circle": "mouse_right",
                   "square": "key:Space", "triangle": "key:Tab", "l1": "key:PageUp",
                   "r1": "key:PageDown", "l2": "mouse_middle", "r2": "key:LShift",
                   "start": "key:Escape", "select": "key:F10", "l3": "none", "r3": "none",
                   "ps": "action:settings"}
# [controls.native] axes: the six DirectInput/XInput axis names a physical
# axis maps to.
NATIVE_AXES = ("x", "y", "z", "rx", "ry", "rz")
# Mirrors the name column of host/controls/layout.cpp's kScancodes table (its
# scancode_from_name); keep both lists in sync.
KEY_NAMES = (
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "Return",
    "Escape", "Backspace", "Tab", "Space", "Minus", "Equals", "LeftBracket", "RightBracket",
    "Backslash", "Semicolon", "Apostrophe", "Grave", "Comma", "Period", "Slash", "F1", "F2", "F3",
    "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Insert", "Home", "PageUp", "Delete",
    "End", "PageDown", "Right", "Left", "Down", "Up", "LCtrl", "LShift", "LAlt",
)
# key:<name>, mouse_left/right/middle, wheel_up/down, action:<name> or none.
BUTTON_TARGET_RE = re.compile(
    r"^(key:[A-Za-z0-9]+|mouse_(left|right|middle)|wheel_(up|down)|"
    r"action:(settings|system_keyboard|edit_layout)|none)$")
HEAP_END = 0x0e000000        # runtime/x86.h GUEST_HEAP_END; the mods' heap starts there
GUEST_SIZE_DEFAULT = 0x10000000   # runtime/x86.h GUEST_SIZE: the arena, 256 MB unless a module needs more
AUX_REQUIRED_KEYS = ("name", "path", "sha256", "base", "size")


def windows_version(value):
    """Decode major.minor[.build]; keep the historical 9x default and 6.1 SP1."""
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", value):
        raise ValueError("[game] windows_version must be major.minor[.build]")
    parts = [int(part) for part in value.split(".")]
    major, minor = parts[:2]
    build = parts[2] if len(parts) == 3 else {(4, 10): 2222, (6, 1): 7601}.get((major, minor), 0)
    if major > 255 or minor > 255 or build > 32767:
        raise ValueError("[game] windows_version requires byte-sized major/minor and a 15-bit build")
    return major, minor, build, 2 if major >= 5 else 1


def validate_guest_size(value):
    if type(value) is not int or value % 0x10000 or not GUEST_SIZE_DEFAULT <= value <= 0xffff0000:
        raise ValueError("[game] guest_size must be a 64 KB aligned integer in 0x10000000..0xffff0000")
    return value


def validate_heap_base(value, guest_size=GUEST_SIZE_DEFAULT):
    """The heap arena start: page aligned, above the image base, below the arena end."""
    heap_end = guest_size - 0x02000000
    if value % 0x1000 or not (0x00400000 < value < heap_end):
        raise ValueError("[game] heap_base %#x must be page aligned and between 0x00400000 and %#x" % (value, heap_end))
    return value


def load_controls(controls, touch, source):
    """Validate [controls], merging [controls.mapped]/[controls.native] over their
    defaults. `touch` is the already-defaulted [touch] table: its `keypad` knob
    only sets `default_layout` when the game has not named one itself."""
    controls = dict(controls)
    if "default_layout" not in controls:
        controls["default_layout"] = {"auto": "keys", "hidden": "hidden"}[touch["keypad"]]
    if controls["default_layout"] not in CONTROLS_LAYOUTS:
        raise ValueError("%s: [controls] default_layout must be one of %s, not %r"
                         % (source, ", ".join(CONTROLS_LAYOUTS), controls["default_layout"]))
    pad = controls.setdefault("pad", "mapped")
    if pad not in ("native", "mapped", "off"):
        raise ValueError('%s: [controls] pad must be "native", "mapped" or "off", not %r' % (source, pad))

    mapped = dict(MAPPED_DEFAULTS)
    mapped.update(controls.get("mapped", {}))
    unknown = sorted(k for k in mapped if k not in MAPPED_DEFAULTS)
    if unknown:
        raise ValueError("%s: [controls.mapped] may name only %s, not %s"
                         % (source, ", ".join(sorted(MAPPED_DEFAULTS)), ", ".join(unknown)))
    for key, value in mapped.items():
        if key == "cursor_speed":
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                raise ValueError("%s: [controls.mapped] cursor_speed must be a positive integer, not %r"
                                 % (source, value))
        elif key in ("left_stick", "right_stick"):
            if value not in STICK_MODES:
                raise ValueError("%s: [controls.mapped] %s must be one of %s, not %r"
                                 % (source, key, ", ".join(STICK_MODES), value))
        elif key == "dpad":
            if value not in ("arrows", "wasd", "none"):
                raise ValueError('%s: [controls.mapped] dpad must be "arrows", "wasd" or "none", not %r'
                                 % (source, value))
        elif not isinstance(value, str) or not BUTTON_TARGET_RE.match(value):
            raise ValueError("%s: [controls.mapped] %s is not a valid target: %r" % (source, key, value))
        elif value.startswith("key:") and value[len("key:"):] not in KEY_NAMES:
            raise ValueError("%s: [controls.mapped] %s names no key: %r" % (source, key, value))
    controls["mapped"] = mapped

    native = dict(controls.get("native", {}))
    unknown_native = sorted(k for k in native if k not in ("xinput", "dinput", "attached_only", "axes", "buttons"))
    if unknown_native:
        raise ValueError("%s: [controls.native] may name only xinput, dinput, attached_only, axes, buttons, not %s"
                         % (source, ", ".join(unknown_native)))
    native.setdefault("xinput", True)
    native.setdefault("dinput", True)
    native.setdefault("attached_only", False)
    if any(not isinstance(native[key], bool) for key in ("xinput", "dinput", "attached_only")):
        raise ValueError("%s: [controls.native] xinput, dinput and attached_only must be booleans" % source)
    axes = native.setdefault("axes", ["x", "y", "z", "rz", "rx", "ry"])
    if not isinstance(axes, list) or sorted(axes) != sorted(NATIVE_AXES):
        raise ValueError("%s: [controls.native] axes must list all six of %s exactly once, not %r"
                         % (source, ", ".join(NATIVE_AXES), axes))
    buttons = native.setdefault(
        "buttons", ["square", "cross", "circle", "triangle", "l1", "r1", "l2", "r2", "select",
                   "start", "l3", "r3", "ps"])
    if not isinstance(buttons, list) or sorted(buttons) != sorted(PAD_BUTTONS):
        raise ValueError("%s: [controls.native] buttons must list all thirteen of %s exactly once, not %r"
                         % (source, ", ".join(PAD_BUTTONS), buttons))
    controls["native"] = native
    return controls


def load(game_dir):
    """Return the parsed config with `globals` merged in and `dir`/`source` recorded."""
    game_dir = Path(game_dir)
    source = game_dir / "game.toml"
    with source.open("rb") as fh:
        cfg = tomllib.load(fh)
    game = cfg.get("game", {})
    missing = [key for key in REQUIRED_GAME_KEYS if key not in game]
    if missing:
        raise ValueError("%s: missing [game] keys: %s" % (source, ", ".join(missing)))
    game["guest_size"] = validate_guest_size(game.get("guest_size", GUEST_SIZE_DEFAULT))
    game["heap_base"] = validate_heap_base(int(game.get("heap_base", HEAP_BASE_DEFAULT)), game["guest_size"])
    windows_version(game.setdefault("windows_version", "4.10"))
    exe_dir = game.setdefault("exe_dir", "")
    if (not isinstance(exe_dir, str) or (exe_dir and
            (exe_dir.startswith(("/", "\\")) or ":" in exe_dir or
             any(c in ("", ".", "..") for c in exe_dir.replace("\\", "/").split("/"))
             or any(ord(c) < 32 for c in exe_dir)))):
        raise ValueError('%s: [game] exe_dir must be a relative directory inside the game root' % source)
    game["exe_dir"] = exe_dir.replace("\\", "/")
    system = cfg.setdefault("system", {})
    bink = cfg.setdefault("bink", {})
    if (not isinstance(bink, dict) or set(bink) - {"record"}
            or bink.setdefault("record", "legacy") not in ("legacy", "sdk")):
        raise ValueError('%s: [bink] record must be "legacy" or "sdk"' % source)
    if system.setdefault("mm_timers", "message_loop") not in ("message_loop", "thread"):
        raise ValueError('%s: [system] mm_timers must be "message_loop" or "thread"' % source)
    desktop = system.setdefault("desktop", "1024x768")
    if (not isinstance(desktop, str) or not re.fullmatch(r"[0-9]+x[0-9]+", desktop)
            or any(not 1 <= int(v) <= 16384 for v in desktop.split("x"))):
        raise ValueError('%s: [system] desktop must be WIDTHxHEIGHT, each between 1 and 16384' % source)
    memory = system.setdefault("memory_mb", 512)
    if type(memory) is not int or not 1 <= memory <= 1048576:
        raise ValueError('%s: [system] memory_mb must be an integer between 1 and 1048576' % source)
    adapter = cfg.setdefault("d3d9", {}).setdefault("adapter", {})
    if not isinstance(adapter, dict):
        raise ValueError('%s: [d3d9] adapter must be a table' % source)
    for key, default, limit in (("vendor_id", 0x10de, 0xffff), ("device_id", 0x0091, 0xffff),
                                ("video_memory_mb", 32, 4095)):
        value = adapter.setdefault(key, default)
        if type(value) is not int or not (1 if key == "video_memory_mb" else 0) <= value <= limit:
            raise ValueError('%s: [d3d9.adapter] %s must be an integer in range' % (source, key))
    description = adapter.setdefault("description", "NVIDIA GeForce 7800 GTX")
    if (not isinstance(description, str) or not description or len(description.encode("utf-8")) > 511
            or any(ord(c) < 32 for c in description)):
        raise ValueError('%s: [d3d9.adapter] description must be 1..511 UTF-8 bytes without control characters' % source)
    translate = cfg.setdefault("translate", {})
    ranges = translate.setdefault("untranslated_ranges", [])
    if not isinstance(ranges, list):
        raise ValueError("%s: [translate] untranslated_ranges must be a list of tables" % source)
    previous_end = 0
    for entry in ranges:
        if (not isinstance(entry, dict) or set(entry) - {"start", "end", "on_branch"}
                or type(entry.get("start")) is not int or type(entry.get("end")) is not int
                or not 0 <= entry["start"] < entry["end"] <= 0x100000000
                or entry.get("on_branch", "fail") not in ("fail", "trap")):
            raise ValueError("%s: [translate] untranslated_ranges requires disjoint 32-bit start/end ranges and on_branch fail/trap" % source)
        entry.setdefault("on_branch", "fail")
    for entry in sorted(ranges, key=lambda r: r["start"]):
        if entry["start"] < previous_end:
            raise ValueError("%s: [translate] untranslated_ranges must not overlap" % source)
        previous_end = entry["end"]
    intrinsics = translate.get("intrinsics")
    if intrinsics is not None:
        if not isinstance(intrinsics, dict) or (intrinsics and set(intrinsics) != {"setjmp", "longjmp"}):
            raise ValueError("%s: [translate] intrinsics must name the setjmp/longjmp pair, or be empty" % source)
        if any(type(value) is not int or not 0 < value <= 0xffffffff for value in intrinsics.values()):
            raise ValueError("%s: [translate] intrinsic addresses must be nonzero 32-bit integers" % source)
        if len(set(intrinsics.values())) != len(intrinsics):
            raise ValueError("%s: [translate] intrinsic addresses must be distinct" % source)
    resumable = translate.setdefault("resumable_stacks", False)
    if not isinstance(resumable, bool):
        raise ValueError("%s: [translate] resumable_stacks must be a boolean" % source)
    alignment = translate.setdefault("function_alignment", 16)
    if type(alignment) is not int or alignment <= 0:
        raise ValueError("%s: [translate] function_alignment must be a positive integer" % source)
    tracks = cfg.setdefault("media", {}).setdefault("cd_tracks", [])
    if not isinstance(tracks, list) or not all(isinstance(v, str) for v in tracks):
        raise ValueError("%s: [media] cd_tracks must be a list of strings" % source)
    cfg.setdefault("hooks", {})
    cfg.setdefault("bundle", {}).setdefault("exclude", [])
    touch = cfg.setdefault("touch", {})
    touch.setdefault("keypad", "auto")
    if touch["keypad"] not in ("auto", "hidden"):
        raise ValueError('%s: [touch] keypad must be "auto" or "hidden", not %r' % (source, touch["keypad"]))
    cfg["controls"] = load_controls(cfg.get("controls", {}), touch, source)
    settings = cfg.setdefault("settings", {})
    rows = settings.setdefault("rows", list(SETTINGS_ROWS))
    if not isinstance(rows, list):
        raise ValueError("%s: [settings] rows must be a list, not %r" % (source, rows))
    rows = [SETTINGS_ROW_ALIASES.get(row, row) for row in rows]
    unknown = [row for row in rows if row not in SETTINGS_ROWS]
    if unknown:
        raise ValueError("%s: [settings] rows may name only %s, not %s"
                         % (source, ", ".join(SETTINGS_ROWS), ", ".join(map(repr, unknown))))
    settings["rows"] = rows
    launcher = cfg.setdefault("launcher", {})
    launcher.setdefault("title", game["name"])
    launcher.setdefault("store", "")
    for key in ("install_names", "gog_ids", "steam_ids"):
        value = launcher.setdefault(key, [])
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise ValueError("%s: [launcher] %s must be a list of strings" % (source, key))
    for key in ("title", "store"):
        if not isinstance(launcher[key], str):
            raise ValueError("%s: [launcher] %s must be a string" % (source, key))
    min_free = launcher.setdefault("min_free_mb", 0)
    if not isinstance(min_free, int) or min_free < 0:
        raise ValueError("%s: [launcher] min_free_mb must be a non-negative integer" % source)
    setup_dirs = cfg.setdefault("setup", {}).setdefault("required_dirs", [])
    if not isinstance(setup_dirs, list):
        raise ValueError("%s: [setup] required_dirs must be a list" % source)
    globals_path = game_dir / translate.get("globals", "globals.toml")
    with globals_path.open("rb") as fh:
        cfg["globals"] = tomllib.load(fh).get("globals", {})
    cfg["dir"] = game_dir
    cfg["source"] = str(source)
    # Developer inputs live beside game.toml: a game repository holds its own
    # ignored original/ and analysis/ directories.
    cfg["developer_exe_path"] = (game_dir / game["developer_exe"]).resolve()
    developer_root = cfg["developer_exe_path"].parent
    for component in reversed(game["exe_dir"].split("/")) if game["exe_dir"] else ():
        if developer_root.name.casefold() != component.casefold():
            raise ValueError('%s: [game] developer_exe must sit inside exe_dir' % source)
        developer_root = developer_root.parent
    cfg["developer_game_dir_path"] = developer_root
    cfg["listings_path"] = (game_dir / translate.get("listings", "analysis")).resolve()
    # [translate] overrides: a header the generated sources include before they
    # define FN_<addr>, so a game can replace one translated function with a
    # native one (translate.py's RECOMP_OVERRIDE_HEADER). Absent by default,
    # and required to exist when named: a path that silently does not resolve
    # would leave the build looking replaced while running the original.
    overrides = translate.get("overrides")
    cfg["overrides_header"] = None
    if overrides is not None:
        path = (game_dir / overrides).resolve()
        if not path.is_file():
            raise ValueError("%s: [translate] overrides names no file: %s" % (source, path))
        cfg["overrides_header"] = path
    cfg["aux_modules"] = load_aux_modules(cfg, game_dir, source)
    return cfg


def load_aux_modules(cfg, game_dir, source):
    """[modules.aux.<key>]: a DLL the guest loads at run time (LoadLibrary) that
    the kit translates as a second image and maps at its preferred base, so
    its code runs as translated code and its exports answer GetProcAddress.
    Keys: name (the file name the guest asks for), path (developer copy,
    relative to game.toml), sha256, base and size (the load address and reserved
    image size), listings (Ghidra export directory, relative), and
    function_alignment (default 4). Modules must not overlap, must be 64 KB
    aligned, and must fit below the heap or above all runtime allocations.
    High modules cap the runtime layout, independently of the arena size."""
    game = cfg["game"]
    guest_size = validate_guest_size(game.setdefault("guest_size", GUEST_SIZE_DEFAULT))
    game["guest_size"] = guest_size
    modules = []
    for key, entry in sorted(cfg.get("modules", {}).get("aux", {}).items()):
        missing = [k for k in AUX_REQUIRED_KEYS if k not in entry]
        if missing:
            raise ValueError("%s: [modules.aux.%s] missing keys: %s" % (source, key, ", ".join(missing)))
        base, size = entry["base"], entry["size"]
        if (type(base) is not int or type(size) is not int or base < 0x10000 or base % 0x10000
                or size <= 0 or base + size > guest_size
                or base < game["heap_base"] < base + size
                or (base + size > game["heap_base"] and base < GUEST_SIZE_DEFAULT)):
            raise ValueError("%s: [modules.aux.%s] base/size must be 64 KB aligned at or above 0x10000, fit the arena, and avoid runtime regions"
                             % (source, key))
        if any(base < m["base"] + m["size"] and m["base"] < base + size for m in modules):
            raise ValueError("%s: [modules.aux.%s] overlaps another module" % (source, key))
        alignment = entry.get("function_alignment", 4)
        if type(alignment) is not int or alignment <= 0:
            raise ValueError("%s: [modules.aux.%s] function_alignment must be a positive integer" % (source, key))
        entries = entry.get("entry_points", [])
        if not isinstance(entries, list) or any(type(a) is not int or not base <= a < base + size
                                               for a in entries):
            raise ValueError("%s: [modules.aux.%s] entry_points must be addresses inside the module"
                             % (source, key))
        modules.append({
            "key": key, "name": entry["name"], "sha256": entry["sha256"], "base": base, "size": size,
            "path": (game_dir / entry["path"]).resolve(),
            "listings_path": (game_dir / entry.get("listings", "analysis/" + entry["name"])).resolve(),
            "function_alignment": alignment,
            "entry_points": entries,
        })
    # Keep every allocating region (including mod memory, stacks and shims)
    # below high DLLs. Enlarging the arena to map a DLL must not move a shim
    # table or a stack into that very DLL's preferred address range.
    game["layout_end"] = min([guest_size] + [m["base"] for m in modules
                                            if m["base"] >= game["heap_base"]])
    validate_heap_base(game["heap_base"], game["layout_end"])
    return modules
