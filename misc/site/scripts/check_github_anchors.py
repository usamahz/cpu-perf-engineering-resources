"""Check that the site's heading anchors are the ones GitHub renders.

Every in-page id the site gives a README heading is computed the way
misc/scripts/check_format.py computes GitHub's anchors, so that a link to
README.md#some-heading lands on the right page. This script asks GitHub
itself: it renders README.md through the REST API's /markdown endpoint and
compares the anchors GitHub generates with the corpus's. A heading with an
unusual character (an underscore, an ampersand, an emoji) is where the two
could part.

A network failure only warns: the check is about GitHub's renderer, not
about the commit being built.

    python misc/site/scripts/check_github_anchors.py --export misc/site/build/export.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
API = "https://api.github.com/markdown"


def github_anchors(text: str, repo: str) -> set[str]:
    body = json.dumps({"text": text, "mode": "gfm", "context": repo}).encode()
    req = urllib.request.Request(API, data=body, method="POST", headers={
        "Accept": "application/vnd.github+json", "Content-Type": "application/json",
        "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "cpu-perf-site-anchor-check",
    })
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=60) as r:
        html = r.read().decode("utf-8")
    ids = set(re.findall(r'\bid="user-content-([^"]+)"', html))
    ids |= set(re.findall(r'<a[^>]+\bhref="#([^"]+)"[^>]*class="anchor"', html))
    return ids


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--export", type=Path, default=ROOT / "misc" / "site" / "build" / "export.json")
    ap.add_argument("--repo", default="usamahz/cpu-performance-engineering")
    args = ap.parse_args(argv)
    export = json.loads(args.export.read_text(encoding="utf-8"))
    ours = {h[3] for h in export["corpus"]["headings"]}
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    try:
        theirs = github_anchors(readme, args.repo)
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        print(f"::warning::GitHub's markdown API was not reachable ({exc}); anchors not compared")
        return 0
    missing = sorted(ours - theirs)
    for anchor in missing:
        print(f"::error::README heading anchor #{anchor} is not one GitHub renders")
    print(f"{len(ours)} README anchors, {len(ours) - len(missing)} match GitHub's renderer")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
