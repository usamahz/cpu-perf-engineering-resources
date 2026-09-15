#!/usr/bin/env python3
"""Paste results/summary.md into README.md between the results markers.

Usage: refresh_readme.py <benchmark dir>
"""
import sys
from pathlib import Path

START, END = "<!-- results:start -->", "<!-- results:end -->"


def main() -> int:
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    readme, summary = d / "README.md", d / "results" / "summary.md"
    text = readme.read_text(encoding="utf-8")
    if START not in text or END not in text:
        print(f"{readme}: missing results markers", file=sys.stderr)
        return 1
    head, rest = text.split(START, 1)
    _old, tail = rest.split(END, 1)
    body = summary.read_text(encoding="utf-8").strip()
    readme.write_text(f"{head}{START}\n{body}\n{END}{tail}", encoding="utf-8")
    print(f"updated {readme.relative_to(d.parent.parent)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
