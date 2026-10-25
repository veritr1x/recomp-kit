"""The packaged macOS app has the narrow hardened-runtime entitlement set."""
import importlib.util
from pathlib import Path
import plistlib
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("finish_bundle", ROOT / "tools/recomp/finish_bundle.py")
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


def test_hardened_signature_and_entitlements():
    entitlement_path = ROOT / "host/macos.entitlements"
    assert plistlib.loads(entitlement_path.read_bytes()) == {
        "com.apple.security.cs.disable-library-validation": True}
    with patch.object(bundle.subprocess,"run") as run:
        bundle.sign_bundle(Path("App.app"))
    sign, verify = [call.args[0] for call in run.call_args_list]
    assert sign[sign.index("--options")+1] == "runtime"
    assert sign[sign.index("--entitlements")+1] == str(entitlement_path)
    assert "--verify" in verify and "--strict" in verify
