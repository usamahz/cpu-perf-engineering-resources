"""python -m cpu_perf_site build | check | serve"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import SITE_ROOT
from .config import load_config
from .data import load_site


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="cpu_perf_site", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "check", "serve"):
        p = sub.add_parser(name)
        p.add_argument("--data", default=str(SITE_ROOT / "build"), help="directory holding export.json")
        p.add_argument("--out", default=str(SITE_ROOT / "dist"), help="output directory")
        p.add_argument("--base-url", default=None, help="absolute site URL, e.g. https://cpu-perf.com")
    sub.choices["serve"].add_argument("--port", type=int, default=8000)
    sub.choices["check"].add_argument("--strict", action="store_true", help="treat warnings as failures")
    args = ap.parse_args(argv)

    config = load_config(args.base_url)
    data, out = Path(args.data), Path(args.out)

    if args.cmd in ("build", "serve"):
        from .pages import Builder

        site = load_site(data, config)
        pages = Builder(site, out).build()
        print(f"built {len(pages)} pages into {out} for {config.base_url}")
        if args.cmd == "serve":
            from .serve import serve

            serve(out, args.port, rebuild=lambda: Builder(load_site(data, load_config(args.base_url)), out).build())
        return 0

    from .check import run_checks

    site = load_site(data, config)
    return run_checks(site, out, strict=args.strict)


if __name__ == "__main__":
    sys.exit(main())
