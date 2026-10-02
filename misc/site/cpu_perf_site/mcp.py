"""The MCP server as the site shows it: its own README, split into sections,
and the tool, resource and prompt lists the build dumped from the running
server (build/mcp-surface.json). Nothing here describes the server in the
site's own words; every MCP page quotes one of those two sources."""

from __future__ import annotations

import re
from dataclasses import dataclass, field

from .markdown import Markdown
from .paths import slugify

SOURCE = "misc/mcp/README.md"
HEADING = re.compile(r"^(#{1,6})\s+(.+?)\s*$")
EXAMPLES_INTRO = re.compile(r"ask, for example", re.I)


@dataclass
class DocSection:
    level: int
    title: str
    anchor: str
    body: str
    children: list = field(default_factory=list)


def split_sections(text: str) -> tuple[str, list[DocSection]]:
    """(text before the first H2, [H2 sections with their H3 children])."""
    intro: list[str] = []
    top: list[DocSection] = []
    current: DocSection | None = None
    buf: list[str] = []
    fence = False

    def flush():
        if current is not None:
            current.body = "\n".join(buf).strip("\n")

    for line in text.splitlines():
        if line.startswith("```"):
            fence = not fence
        m = None if fence else HEADING.match(line)
        if m and len(m.group(1)) in (2, 3):
            flush()
            buf = []
            level, title = len(m.group(1)), m.group(2)
            sec = DocSection(level, title, slugify(title), "")
            if level == 2 or not top:
                top.append(sec)
            else:
                top[-1].children.append(sec)
            current = sec
            continue
        if m and len(m.group(1)) == 1:
            continue  # the README's own title
        (buf if current is not None else intro).append(line)
    flush()
    return "\n".join(intro).strip(), top


class McpView:
    def __init__(self, readme: str, surface: dict | None, md: Markdown):
        self.readme = readme
        self.surface = surface or None
        self.md = md
        self.intro_md, self.sections = split_sections(readme)
        self.examples = self._examples()

    # ---- README -------------------------------------------------------------

    def find(self, anchor: str) -> DocSection | None:
        for s in self.sections:
            if s.anchor == anchor:
                return s
            for c in s.children:
                if c.anchor == anchor:
                    return c
        return None

    def render(self, text: str, demote: int = 0) -> str:
        return self.md.render(text, SOURCE, demote=demote)

    def section_html(self, anchor: str, demote: int = 0, children: bool = True) -> str:
        s = self.find(anchor)
        if s is None:
            return ""
        parts = [s.body]
        if children:
            for c in s.children:
                parts.append(f"{'#' * c.level} {c.title}\n\n{c.body}")
        return self.render("\n\n".join(parts), demote=demote)

    @property
    def intro_html(self) -> str:
        return self.render(self.intro_md)

    def _examples(self) -> list[str]:
        out, on = [], False
        for line in self.readme.splitlines():
            if EXAMPLES_INTRO.search(line):
                on = True
                continue
            if on:
                if line.startswith("- "):
                    out.append(line[2:].strip())
                elif out and not line.strip():
                    break
        return out

    def example_html(self, text: str) -> str:
        return self.md.inline(text, SOURCE)

    # ---- surface ------------------------------------------------------------

    @property
    def tools(self) -> list[dict]:
        return list((self.surface or {}).get("tools", []))

    @property
    def prompts(self) -> list[dict]:
        return list((self.surface or {}).get("prompts", []))

    @property
    def resources(self) -> list[dict]:
        return list((self.surface or {}).get("resources", []))

    @property
    def templates(self) -> list[dict]:
        return list((self.surface or {}).get("resource_templates", []))

    @property
    def server(self) -> dict:
        return (self.surface or {}).get("server", {})

    def prompt(self, name: str) -> dict | None:
        return next((p for p in self.prompts if p.get("name") == name), None)

    def template(self, prefix: str) -> dict | None:
        return next((t for t in self.templates if t.get("uriTemplate", "").startswith(prefix)), None)
