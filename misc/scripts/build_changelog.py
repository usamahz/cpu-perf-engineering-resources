#!/usr/bin/env python3
"""Build misc/notes/changelog.md from the drafts in misc/notes/sections/.

For every section file it counts the entries that made it into the list
and copies the '## Rejected' block (title, URL, rule failed) and the
'## Claims' block (numbers examined against the seven-field rule), so the
public record of what was considered and why it was left out is generated
from the same files the README was assembled from.
"""

from __future__ import annotations

import re
import sys
from datetime import date
from pathlib import Path

MISC = Path(__file__).resolve().parents[1]  # misc/scripts/ -> misc/
SECTIONS = MISC / "notes" / "sections"
OUT = MISC / "notes" / "changelog.md"
ENTRY = re.compile(r"^(?:- |\d+\. )\[[^\]]+\]\(https?://[^\s)]+\) - ")
STOP = ("## Rejected", "## Claims", "## Link notes", "## Benchmark proposal")


def block(text: str, name: str) -> list[str]:
    lines = text.splitlines()
    out: list[str] = []
    active = False
    for line in lines:
        if line.startswith("## "):
            active = line.strip() == name
            continue
        if active and line.startswith("- "):
            out.append(line)
    return out


def included(text: str) -> int:
    n = 0
    for line in text.splitlines():
        if any(line.startswith(s) for s in STOP):
            break
        if ENTRY.match(line):
            n += 1
    return n


def main() -> int:
    files = sorted(SECTIONS.glob("[0-9][0-9]-*.md"))
    if not files:
        print("no section files", file=sys.stderr)
        return 1
    out = [
        "# Changelog of inclusion and rejection",
        "",
        f"Generated on {date.today().isoformat()} by `misc/scripts/build_changelog.py` from the",
        "section drafts in `misc/notes/sections/`. Each section lists what was considered",
        "and left out, with the rule it failed, and every performance number that was",
        "examined against the seven-field rule with its verdict. Entries that made the",
        "list are in the README.",
        "",
    ]
    total_in = total_out = total_claims = 0
    for f in files:
        text = f.read_text(encoding="utf-8")
        title = next((l[3:] for l in text.splitlines() if l.startswith("## ")), f.stem)
        n_in = included(text)
        rejected = block(text, "## Rejected")
        claims = block(text, "## Claims")
        total_in += n_in
        total_out += len(rejected)
        total_claims += len(claims)
        out += [f"## {title}", "", f"Included: {n_in}. Rejected: {len(rejected)}. Numbers examined: {len(claims)}.", ""]
        if rejected:
            out += ["### Rejected", ""] + rejected + [""]
        if claims:
            out += ["### Numbers examined", ""] + claims + [""]
    out.insert(8, f"Totals: {total_in} entries included, {total_out} rejected, {total_claims} numbers examined.")
    out.insert(9, "")
    OUT.write_text("\n".join(out).rstrip() + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(MISC.parent)}: {total_in} included, {total_out} rejected, {total_claims} claims")
    return 0


if __name__ == "__main__":
    sys.exit(main())
