#!/usr/bin/env python3
"""Re-run discovery with configured seeds removed from an in-memory scratch config.

Usage: python tools/recomp/tests/probe_discovery.py --game games/example
       [--addresses 00401000,00402000] [--out build/probe-discovery]
The game's files and game.toml are never modified. Output is a disposable
translation; diagnostics for every selected address are written to stderr.
"""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import translate as T  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game",required=True)
    parser.add_argument("--addresses",help="comma-separated hexadecimal addresses; default: configured seeds")
    parser.add_argument("--out",default="build/probe-discovery")
    args = parser.parse_args()
    cfg = T.game_config.load(args.game)
    selected = ({int(a,16) for a in args.addresses.split(",")} if args.addresses else
                set(cfg["translate"].get("entry_points",())))
    if not selected:
        parser.error("no addresses selected")
    addresses = ",".join("%08x" % a for a in sorted(selected))
    configure = T.configure

    def scratch_configure(config):
        configure(config)
        T.EXTRA_ENTRY_POINTS -= selected

    T.configure = scratch_configure
    os.environ["RECOMP_TRACE_DISCOVERY"] = addresses
    sys.argv = ["translate.py","--game",args.game,"--forget",addresses,"--out",args.out]
    return T.main()


if __name__ == "__main__":
    raise SystemExit(main())
