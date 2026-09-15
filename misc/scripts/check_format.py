#!/usr/bin/env python3
"""Lint the README (or any list file) for the one-line entry format.

Usage: misc/scripts/check_format.py [FILE ...]      (default: README.md)

Every resource entry must be exactly one line of the form

    - [Title](https://...) - Why it earns its place.

or, inside the Start here section,

    1. [Title](https://...) - Why it earns its place.

Rules enforced:
  * entry lines match the form above; the reason ends with a full stop and
    is at most 160 characters
  * Watchlist (Frontier) lines may omit the link but must state a promotion condition
    (contain "until", "pending", "once " or "when ")
  * no em dashes, no TODO/TBD/FIXME/placeholder text anywhere
  * every internal anchor resolves to a heading (GitHub anchor rules)
  * if a "## Contents" section exists, every H2 and H3 heading below it is
    listed there and every listed anchor exists
  * every H3 in a resource section has at least one entry
  * no URL appears twice inside the same subsection
  * the Start here list is numbered 1..N without gaps

Exit status 1 on any error.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*$")
ENTRY = re.compile(r"^(?:- |(\d+)\. )\[(?P<title>[^\]]+)\]\((?P<url>https?://[^\s)]+)\) - (?P<reason>\S.*)$")
WATCH = re.compile(r"^- (?:\[(?P<title>[^\]]+)\]\((?P<url>https?://[^\s)]+)\) - )?(?P<reason>\S.*)$")
TOC_LINE = re.compile(r"^\s*- \[(?P<text>[^\]]+)\]\(#(?P<anchor>[^)]+)\)\s*$")
ANCHOR_LINK = re.compile(r"\]\(#([^)]+)\)")
RELATIVE_LINK = re.compile(r"\]\(([^)\s]+)\)")
FORBIDDEN = [
    ("—", "em dash"),
    ("TODO", "TODO"),
    ("TBD", "TBD"),
    ("FIXME", "FIXME"),
    ("coming soon", "placeholder text"),
    ("placeholder", "placeholder text"),
    ("lorem ipsum", "placeholder text"),
]
NON_RESOURCE = {
    "contents",
    "source policy",
    "license",
    "maintainer",
    "rejected",
    "claims",
    "link notes",
    "benchmark proposal",
    "how to use this list",
    "about",
}
MAX_REASON = 160


def github_anchor(title: str) -> str:
    title = re.sub(r"<[^>]+>", "", title)
    title = re.sub(r"[`*_~]", "", title).strip().lower()
    title = re.sub(r"[^\w\- ]", "", title)
    return re.sub(r"[ ]+", "-", title)


def lint(path: Path) -> list[str]:
    errors: list[str] = []
    lines = path.read_text(encoding="utf-8").splitlines()
    name = path.name

    # Pass 1: headings and anchors.
    anchors: dict[str, int] = {}
    headings: list[tuple[int, int, str, str]] = []  # (line, level, title, anchor)
    in_code = False
    for n, line in enumerate(lines, 1):
        if line.strip().startswith("```"):
            in_code = not in_code
            continue
        if in_code:
            continue
        m = HEADING.match(line)
        if not m:
            continue
        base = github_anchor(m.group(2))
        count = anchors.get(base, 0)
        anchors[base] = count + 1
        anchor = base if count == 0 else f"{base}-{count}"
        headings.append((n, len(m.group(1)), m.group(2), anchor))
    anchor_set = {h[3] for h in headings}

    # Pass 2: line-level checks.
    current_h2 = ""
    current_h3 = ""
    section_key = ""
    section_urls: dict[str, set[str]] = {}
    section_entries: dict[str, int] = {}
    h3_lines: dict[str, int] = {}
    start_numbers: list[int] = []
    in_code = False
    toc_anchors: list[tuple[int, str]] = []
    in_toc = False

    def is_resource(h2: str) -> bool:
        plain = re.sub(r"^\d+\.\s+", "", h2).strip().lower()
        return plain not in NON_RESOURCE

    for n, line in enumerate(lines, 1):
        if line.strip().startswith("```"):
            in_code = not in_code
            continue
        if in_code:
            continue
        for needle, label in FORBIDDEN:
            if needle in line:
                errors.append(f"{name}:{n}: {label} found")
        if line != line.rstrip():
            errors.append(f"{name}:{n}: trailing whitespace")

        m = HEADING.match(line)
        if m:
            level = len(m.group(1))
            title = m.group(2)
            if level == 2:
                current_h2, current_h3 = title, ""
                in_toc = title.strip().lower() == "contents"
            elif level == 3:
                current_h3 = title
                if is_resource(current_h2):
                    h3_lines[f"{current_h2} / {current_h3}"] = n
            section_key = f"{current_h2} / {current_h3}" if current_h3 else current_h2
            continue

        if in_toc:
            t = TOC_LINE.match(line)
            if t:
                toc_anchors.append((n, t.group("anchor")))
            elif line.strip().startswith("-"):
                errors.append(f"{name}:{n}: malformed Contents line")
            continue

        for a in ANCHOR_LINK.findall(line):
            if a not in anchor_set:
                errors.append(f"{name}:{n}: broken internal anchor #{a}")
        for target in RELATIVE_LINK.findall(line):
            if target.startswith(("http://", "https://", "#", "mailto:")):
                continue
            rel = target.split("#", 1)[0]
            if rel and not (path.parent / rel).exists():
                errors.append(f"{name}:{n}: relative link target does not exist: {target}")

        stripped = line.strip()
        is_bullet = stripped.startswith("- ") or re.match(r"^\d+\. ", stripped)
        if not is_bullet or not current_h2 or not is_resource(current_h2):
            continue
        if line.startswith((" ", "\t")):
            errors.append(f"{name}:{n}: nested bullets are not allowed in resource sections")
            continue

        watchlist = "watchlist" in current_h2.lower() or current_h2.strip().lower() == "frontier"
        m = ENTRY.match(line)
        if watchlist and not m:
            w = WATCH.match(line)
            if not w:
                errors.append(f"{name}:{n}: watchlist line must be '- [Title](URL) - text.' or '- text.'")
                continue
            reason = w.group("reason")
            if not re.search(r"\b(until|pending|once|when)\b", reason):
                errors.append(f"{name}:{n}: watchlist line must state the promotion condition (until/pending/once/when)")
            if not reason.endswith("."):
                errors.append(f"{name}:{n}: watchlist line must end with a full stop")
            url = w.group("url")
        elif not m:
            errors.append(f"{name}:{n}: entry must be '- [Title](URL) - Reason.' on one line")
            continue
        else:
            reason = m.group("reason")
            url = m.group("url")
            if not reason.endswith("."):
                errors.append(f"{name}:{n}: reason must end with a full stop")
            if len(reason) > MAX_REASON:
                errors.append(f"{name}:{n}: reason is {len(reason)} characters, limit {MAX_REASON}")
            if "  " in reason:
                errors.append(f"{name}:{n}: double space in reason")
            if m.group(1) is not None:
                if "start here" not in current_h2.lower():
                    errors.append(f"{name}:{n}: numbered entries are only allowed under Start here")
                start_numbers.append(int(m.group(1)))
            elif "start here" in current_h2.lower():
                errors.append(f"{name}:{n}: Start here entries must be numbered")

        section_entries[section_key] = section_entries.get(section_key, 0) + 1
        if url:
            seen = section_urls.setdefault(section_key, set())
            key = url.rstrip("/")
            if key in seen:
                errors.append(f"{name}:{n}: duplicate URL in {section_key!r}: {url}")
            seen.add(key)

    for key, n in h3_lines.items():
        if section_entries.get(key, 0) == 0:
            errors.append(f"{name}:{n}: subsection {key!r} has no entries")

    if start_numbers and start_numbers != list(range(1, len(start_numbers) + 1)):
        errors.append(f"{name}: Start here numbering is {start_numbers}, expected 1..{len(start_numbers)}")

    if any(h[2].strip().lower() == "contents" and h[1] == 2 for h in headings):
        listed = {a for _, a in toc_anchors}
        for n, a in toc_anchors:
            if a not in anchor_set:
                errors.append(f"{name}:{n}: Contents points at missing heading #{a}")
        toc_line = next(h[0] for h in headings if h[2].strip().lower() == "contents" and h[1] == 2)
        for n, level, title, anchor in headings:
            if n <= toc_line or level not in (2, 3):
                continue
            if title.strip().lower() in {"license", "maintainer"}:
                continue
            if anchor not in listed:
                errors.append(f"{name}:{n}: heading {title!r} is missing from Contents")

    return errors


def main() -> int:
    root = Path(__file__).resolve().parents[2]  # misc/scripts/ -> repository root
    files = sys.argv[1:] or ["README.md"]
    all_errors: list[str] = []
    for f in files:
        p = Path(f) if Path(f).is_absolute() else root / f
        if not p.exists():
            all_errors.append(f"missing file: {p}")
            continue
        all_errors += lint(p)
    if all_errors:
        print("Format checks failed:")
        for e in all_errors:
            print(f"- {e}")
        return 1
    print(f"Format checks passed for {', '.join(files)}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
