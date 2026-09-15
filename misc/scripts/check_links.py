#!/usr/bin/env python3
"""Check that every external URL in the given markdown files is live.

Usage: misc/scripts/check_links.py [--json REPORT] [--report REPORT.md] [FILE ...]

Defaults to README.md and CONTRIBUTING.md. Exit status is 1 when any link
is dead. Dead means the resource is gone: HTTP 404 or 410, another 4xx
that is not a bot block, or a host name that no longer resolves. A
network failure that says nothing about the resource (a timeout, a
connection refused or reset, a route that is unreachable from this
machine, a 5xx that persists across retries) is reported as
"unreachable" and does not fail the run, because a slow or IPv6-only host
seen from a CI runner is not a dead link; pass --fail-unreachable to make
it one. A 401, 403 or 429 is reported as "blocked" and does not fail the
run, because several primary sources (vendor document libraries,
publisher sites) refuse automated clients while serving the page to a
browser; those are listed so a human can spot-check. A permanent redirect
(301 or 308) to a different document is reported as "moved" and does not
fail the run, but the new location is printed so the README can be
updated to the canonical URL.

IPv4 addresses are tried before IPv6 ones, because GitHub-hosted runners
have no IPv6 route and would otherwise report every dual-stack host as
unreachable.

Standard library only. No network access other than the links themselves.
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path

LINK = re.compile(r"\[[^\]]*\]\((https?://[^\s)]+)\)")
BARE = re.compile(r"<(https?://[^>\s]+)>")
USER_AGENT = (
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0 Safari/537.36 cpu-perf-link-check/1.0"
)
TIMEOUT = 25
RETRIES = 3
WORKERS = 12
BLOCKED = {401, 403, 429}
DEAD = {404, 410}


_real_getaddrinfo = socket.getaddrinfo


def _ipv4_first_getaddrinfo(*args, **kwargs):
    """Order resolved addresses so IPv4 is tried first; IPv6 stays as fallback."""
    results = _real_getaddrinfo(*args, **kwargs)
    return sorted(results, key=lambda r: 0 if r[0] == socket.AF_INET else 1)


socket.getaddrinfo = _ipv4_first_getaddrinfo


@dataclass
class Result:
    url: str
    status: str  # ok | blocked | moved | unreachable | dead
    code: int | None
    final_url: str
    detail: str
    files: list[str]


class _Redirect(Exception):
    def __init__(self, code: int, location: str) -> None:
        super().__init__(f"{code} -> {location}")
        self.code = code
        self.location = location


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise _Redirect(code, newurl)


def _open(url: str, method: str, verify: bool = True, timeout: float = TIMEOUT) -> tuple[int | None, int, str]:
    """Follow redirects by hand. Returns (first_redirect_code, final_code, final_url)."""
    ctx = ssl.create_default_context()
    if not verify:
        # Only used to classify a server whose TLS chain is incomplete: the
        # document is served, a browser fills in the missing intermediate,
        # but a plain OpenSSL client cannot. Reported as blocked, never ok.
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    opener = urllib.request.build_opener(_NoRedirect(), urllib.request.HTTPSHandler(context=ctx))
    current = url
    first_redirect: int | None = None
    for _hop in range(10):
        req = urllib.request.Request(
            current,
            method=method,
            headers={
                "User-Agent": USER_AGENT,
                "Accept": "text/html,application/pdf,*/*;q=0.8",
                "Accept-Language": "en-GB,en;q=0.8",
            },
        )
        try:
            with opener.open(req, timeout=timeout) as resp:
                return (first_redirect, resp.getcode(), resp.geturl())
        except _Redirect as r:
            if first_redirect is None:
                first_redirect = r.code
            current = urllib.parse.urljoin(current, r.location)
        except urllib.error.HTTPError as e:
            return (first_redirect, e.code, current)
    raise RuntimeError("too many redirects")


def _fetch_once(url: str, timeout: float = TIMEOUT) -> tuple[int | None, int, str]:
    try:
        redirect, code, final = _open(url, "HEAD", timeout=timeout)
        if 200 <= code < 300:
            return redirect, code, final
    except Exception:  # noqa: BLE001 - fall through to GET
        pass
    return _open(url, "GET", timeout=timeout)


def _strip(u: str) -> str:
    p = urllib.parse.urlsplit(u)
    host = p.netloc.lower()
    if host.startswith("www."):
        host = host[4:]
    path = p.path.rstrip("/") or "/"
    return f"{host}{path}?{p.query}"


def check(url: str, files: list[str]) -> Result:
    last_error = ""
    for attempt in range(RETRIES):
        try:
            # Slow hosts get a longer timeout on each retry before they count as dead.
            redirect, code, final = _fetch_once(url, timeout=TIMEOUT * (attempt + 1))
        except urllib.error.URLError as e:
            if isinstance(e.reason, ssl.SSLCertVerificationError):
                try:
                    _r, code, final = _open(url, "GET", verify=False)
                except Exception:  # noqa: BLE001
                    code = 0
                if 200 <= code < 300:
                    return Result(url, "blocked", code, final, "TLS chain incomplete; served without verification", files)
            if isinstance(e.reason, socket.gaierror):
                # The name does not resolve: the host is gone, not merely slow.
                return Result(url, "dead", None, url, f"host does not resolve: {e.reason}", files)
            last_error = f"{type(e).__name__}: {e}"
            time.sleep(1.5 * (attempt + 1))
            continue
        except socket.gaierror as e:
            return Result(url, "dead", None, url, f"host does not resolve: {e}", files)
        except Exception as e:  # noqa: BLE001
            last_error = f"{type(e).__name__}: {e}"
            time.sleep(1.5 * (attempt + 1))
            continue
        if 200 <= code < 300:
            if redirect in (301, 308) and _strip(final) != _strip(url):
                return Result(url, "moved", redirect, final, f"{redirect} to {final}", files)
            return Result(url, "ok", code, final, "", files)
        if code in BLOCKED:
            return Result(url, "blocked", code, final, f"HTTP {code}", files)
        if code in DEAD:
            return Result(url, "dead", code, final, f"HTTP {code}", files)
        if code >= 500:
            last_error = f"HTTP {code}"
            time.sleep(2.0 * (attempt + 1))
            continue
        return Result(url, "dead", code, final, f"HTTP {code}", files)
    # Every attempt failed at the network level or with a 5xx: the resource
    # may well be there, so this is unreachable from here, not dead.
    return Result(url, "unreachable", None, url, last_error or "no response", files)


def collect(paths: list[Path]) -> dict[str, list[str]]:
    urls: dict[str, list[str]] = {}
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for pattern in (LINK, BARE):
            for m in pattern.finditer(text):
                urls.setdefault(m.group(1), []).append(str(path))
    return {u: sorted(set(f)) for u, f in urls.items()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", default=["README.md", "CONTRIBUTING.md"])
    ap.add_argument("--json", help="write full results as JSON to this path")
    ap.add_argument("--report", help="write a markdown report of problems to this path")
    ap.add_argument("--workers", type=int, default=WORKERS)
    ap.add_argument("--fail-unreachable", action="store_true",
                    help="treat unreachable hosts as dead for the exit status")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[2]  # misc/scripts/ -> repository root
    paths = [Path(f) if Path(f).is_absolute() else root / f for f in args.files]
    missing = [p for p in paths if not p.exists()]
    if missing:
        for p in missing:
            print(f"missing file: {p}", file=sys.stderr)
        return 2

    urls = collect(paths)
    print(f"checking {len(urls)} unique links in {', '.join(p.name for p in paths)}")
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        results = list(ex.map(lambda kv: check(kv[0], kv[1]), sorted(urls.items())))

    by_status: dict[str, list[Result]] = {}
    for r in results:
        by_status.setdefault(r.status, []).append(r)

    for status in ("dead", "unreachable", "moved", "blocked"):
        for r in by_status.get(status, []):
            print(f"{status.upper():11} {r.url}  ({r.detail})")

    print(
        f"ok {len(by_status.get('ok', []))}, moved {len(by_status.get('moved', []))}, "
        f"blocked {len(by_status.get('blocked', []))}, "
        f"unreachable {len(by_status.get('unreachable', []))}, dead {len(by_status.get('dead', []))}"
    )

    if args.json:
        Path(args.json).write_text(json.dumps([asdict(r) for r in results], indent=2), encoding="utf-8")

    if args.report:
        lines = ["# Link check report", ""]
        for status, title in (
            ("dead", "Dead links"),
            ("unreachable", "Unreachable from this machine (not counted as dead)"),
            ("moved", "Permanent redirects"),
            ("blocked", "Blocked (verify by hand)"),
        ):
            items = by_status.get(status, [])
            if not items:
                continue
            lines += [f"## {title} ({len(items)})", ""]
            lines += [f"- {r.url} - {r.detail} - in {', '.join(Path(f).name for f in r.files)}" for r in items]
            lines.append("")
        Path(args.report).write_text("\n".join(lines), encoding="utf-8")

    failed = bool(by_status.get("dead")) or (args.fail_unreachable and bool(by_status.get("unreachable")))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
