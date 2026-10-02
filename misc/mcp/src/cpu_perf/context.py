"""Understand pasted tool output, deterministically.

perf stat (plain, -x CSV, -j JSON, per-CPU and interval forms), its top-down
output (old --topdown tables and perf 6 TopdownL1 / tma_* lines), toplev,
gcc and clang vectoriser remarks, assembly and source code. The result is
the counters as read, metrics computed from them with their formulas, notes
on how far to trust them, and the terms that steer the search.

Nothing here judges a number unless a source the list links states the
threshold; the only such thresholds are Intel's own top-down level 1 values,
applied only to Intel P-cores."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field

MAX_CHARS = 100_000
MAX_LINES = 5_000

# Intel's thresholds for top-down level 1, verbatim from TMA_Metrics-full.xlsx
# (sheet TMA_Metrics_5.2-full, column Threshold), the list's entry 6.2.2.
# Fractions of pipeline slots; perf prints percentages.
TMA_SOURCE = (
    "Intel TMA_Metrics-full.xlsx, sheet TMA_Metrics_5.2-full, column Threshold "
    "(https://github.com/intel/perfmon/blob/main/TMA_Metrics-full.xlsx)"
)
TMA_THRESHOLDS = {
    "Frontend_Bound": ("> 0.15", 0.15),
    "Bad_Speculation": ("> 0.15", 0.15),
    "Backend_Bound": ("> 0.2", 0.2),
    "Retiring": ("(> 0.7 | Heavy_Operations)", 0.7),
}
TMA_ROUTING = {
    "Frontend_Bound": "frontend bound instruction fetch decode icache",
    "Bad_Speculation": "bad speculation branch misprediction",
    "Backend_Bound": "backend bound memory bound core bound",
    "Retiring": "retiring vectorization instruction count",
}
TMA_NAMES = {
    "retiring": "Retiring",
    "bad speculation": "Bad_Speculation",
    "bad_speculation": "Bad_Speculation",
    "frontend bound": "Frontend_Bound",
    "frontend_bound": "Frontend_Bound",
    "fe bound": "Frontend_Bound",
    "backend bound": "Backend_Bound",
    "backend_bound": "Backend_Bound",
    "be bound": "Backend_Bound",
}

# perf's generic names and the common raw names, folded to one key each
ALIASES = {
    "cycles": "cycles",
    "cpu-cycles": "cycles",
    "cpu_clk_unhalted.thread": "cycles",
    "cpu_clk_unhalted.core": "cycles",
    "instructions": "instructions",
    "inst_retired.any": "instructions",
    "branches": "branches",
    "branch-instructions": "branches",
    "br_inst_retired.all_branches": "branches",
    "branch-misses": "branch-misses",
    "br_misp_retired.all_branches": "branch-misses",
    "l1-dcache-loads": "L1-dcache-loads",
    "l1-dcache-load-misses": "L1-dcache-load-misses",
    "l1-icache-load-misses": "L1-icache-load-misses",
    "llc-loads": "LLC-loads",
    "llc-load-misses": "LLC-load-misses",
    "cache-references": "cache-references",
    "cache-misses": "cache-misses",
    "stalled-cycles-frontend": "stalled-cycles-frontend",
    "idle-cycles-frontend": "stalled-cycles-frontend",
    "stalled-cycles-backend": "stalled-cycles-backend",
    "idle-cycles-backend": "stalled-cycles-backend",
    "dtlb-loads": "dTLB-loads",
    "dtlb-load-misses": "dTLB-load-misses",
    "itlb-loads": "iTLB-loads",
    "itlb-load-misses": "iTLB-load-misses",
    "task-clock": "task-clock",
    "cpu-clock": "task-clock",
    "context-switches": "context-switches",
    "cs": "context-switches",
    "cpu-migrations": "cpu-migrations",
    "migrations": "cpu-migrations",
    "page-faults": "page-faults",
    "faults": "page-faults",
    "slots": "slots",
    "topdown.slots": "slots",
    "topdown-retiring": "topdown-retiring",
    "topdown-bad-spec": "topdown-bad-spec",
    "topdown-fe-bound": "topdown-fe-bound",
    "topdown-be-bound": "topdown-be-bound",
}

ROUTING = {
    "IPC": "instructions per cycle",
    "branch miss rate": "branch misprediction",
    "branch MPKI": "branch misprediction",
    "L1d miss rate": "cache miss L1",
    "L1d MPKI": "cache miss L1",
    "LLC miss rate": "last level cache miss memory latency",
    "LLC MPKI": "last level cache miss memory latency",
    "cache miss rate": "cache miss",
    "dTLB miss rate": "tlb huge pages",
    "iTLB miss rate": "tlb instruction footprint",
    "frontend stall share": "frontend stalls",
    "backend stall share": "backend stalls memory",
    "context switches per second": "context switches scheduler",
}

# gcc and clang vectoriser reasons -> what to read about
REMARK_REASONS: tuple[tuple[str, str], ...] = (
    (r"complicated access pattern|non-consecutive|strided|gather", "strided access gather data layout structure of arrays"),
    (r"alias|unsafe dependent memory|cannot prove|runtime check|dependence|versioning", "aliasing restrict pointer"),
    (r"control flow|switch|unsupported.*(if|branch)|cannot be if-converted", "control flow branches predication"),
    (r"number of iterations|trip count|loop bounds|array bounds|could not determine", "loop trip count bounds"),
    (r"call|clobbers memory|function", "function call inlining vector math"),
    (r"reduction|floating[- ]point|reorder|fast-math|reassociat|fp ", "floating point reduction reassociation fast-math"),
    (r"data ref|data-ref|unhandled", "pointer analysis data dependence"),
    (r"not beneficial|cost model|cost", "vectorization cost model"),
    (r"unaligned|alignment|peel", "alignment"),
    (r"outer loop|inner-loop|loop nest", "loop nest outer loop"),
)

ASM_FAMILIES: tuple[tuple[str, str], ...] = (
    (r"^v?p?gather|^vpgather|^vgather", "gather"),
    (r"^v?(div|sqrt)", "divider latency throughput"),
    (r"^(xchg|cmpxchg|xadd)", "atomic contention"),
    (r"^pause$", "spin lock"),
    (r"^[lsm]fence$", "memory ordering fences"),
    (r"^prefetch", "software prefetch"),
    (r"^v?fmadd|^v?fmsub|^vfnmadd", "fma throughput"),
    (r"^rdtscp?$", "timing rdtsc"),
)

CODE_PATTERNS: tuple[tuple[str, str], ...] = (
    (r"\b_mm\d*_\w+", "intrinsics simd"),
    (r"\bstd::atomic\b|\batomic_\w+\(|\bmemory_order_\w+", "atomics memory ordering"),
    (r"\balignas\s*\(|__attribute__\s*\(\(\s*aligned", "alignment cache line padding"),
    (r"#\s*pragma\s+omp", "openmp threads"),
    (r"__builtin_prefetch", "software prefetch"),
    (r"__builtin_expect|\[\[(un)?likely\]\]", "branch prediction"),
    (r"\b(malloc|calloc|realloc|free|operator new)\s*\(", "allocator malloc"),
    (r"\b(pthread_mutex_\w+|std::mutex|spin_?lock)\b", "locks contention"),
    (r"\b(__)?restrict\b", "aliasing restrict"),
    (r"#\s*pragma\s+(GCC\s+ivdep|clang\s+loop|omp\s+simd)", "auto-vectorization"),
    (r"\bvolatile\b", "memory ordering"),
)


@dataclass
class Count:
    event: str  # canonical name when known, else as printed (lower case)
    raw: str
    pmu: str = ""
    mods: str = ""
    value: float | None = None
    unit: str = ""
    running: float | None = None  # percentage of the time the counter was on
    status: str = "ok"  # ok | not counted | not supported


@dataclass
class Metric:
    name: str
    value: float
    unit: str = ""
    formula: str = ""
    inputs: list[str] = field(default_factory=list)
    flag: str | None = None
    source: str | None = None


@dataclass
class Analysis:
    kinds: list[str] = field(default_factory=list)
    counts: list[Count] = field(default_factory=list)
    metrics: list[Metric] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    terms: list[str] = field(default_factory=list)  # identifiers worth searching for exactly
    routing: list[str] = field(default_factory=list)  # topic words for the list and library
    remarks: list[str] = field(default_factory=list)
    unparsed: list[str] = field(default_factory=list)
    elapsed: float | None = None
    topics: list[str] = field(default_factory=list)  # words of the list's subsection titles to read first
    flagged: list[tuple[float, str]] = field(default_factory=list)  # (how far over Intel's threshold, topic)

    @property
    def search_terms(self) -> list[str]:
        out: list[str] = []
        for t in self.routing + self.terms:
            if t not in out:
                out.append(t)
        return out[:24]

    def summary(self):
        from .models import ContextOut, MetricOut

        counters = {}
        for c in self.counts:
            if c.value is not None and c.status == "ok":
                key = f"{c.pmu}/{c.event}" if c.pmu else c.event
                key += f":{c.mods}" if c.mods else ""
                counters[key] = counters.get(key, 0.0) + c.value
        return ContextOut(
            kinds=self.kinds,
            metrics=[MetricOut(**m.__dict__) for m in self.metrics],
            counters=dict(list(counters.items())[:40]),
            notes=self.notes,
            terms=self.search_terms,
            remarks=self.remarks[:12],
            unparsed=self.unparsed[:20],
        )


# ----- numbers ------------------------------------------------------------------------


def parse_number(text: str, integer: bool) -> float | None:
    """perf prints counts with the locale's thousands separator."""
    t = text.strip().replace(" ", "").replace(" ", "").replace("'", "").replace(" ", "")
    if not t or not re.fullmatch(r"[\d.,]+", t):
        return None
    if "," in t and "." in t:
        dec = "," if t.rfind(",") > t.rfind(".") else "."
        t = t.replace("." if dec == "," else ",", "").replace(dec, ".")
    elif "," in t:
        t = t.replace(",", "") if (integer or re.fullmatch(r"\d{1,3}(,\d{3})+", t)) else t.replace(",", ".")
    elif "." in t and integer and re.fullmatch(r"\d{1,3}(\.\d{3})+", t):
        t = t.replace(".", "")
    try:
        return float(t)
    except ValueError:
        return None


def split_event(raw: str) -> tuple[str, str, str]:
    """'cpu_core/cycles/u' -> ('cycles', 'cpu_core', 'u'); 'cycles:u' -> ('cycles', '', 'u')."""
    name, pmu, mods = raw.strip(), "", ""
    m = re.fullmatch(r"([\w.-]+)/([^/]+)/(\w*)", name)
    if m:
        pmu, name, mods = m.group(1), m.group(2), m.group(3)
    elif ":" in name:
        name, mods = name.rsplit(":", 1)
    key = ALIASES.get(name.lower(), name.lower())
    return key, pmu, mods


# ----- perf stat --------------------------------------------------------------------------

PLAIN = re.compile(
    r"^\s*(?:(?P<ts>\d+\.\d+)\s+)?"
    r"(?:(?P<where>CPU\d+|S\d+(?:-D\d+)?(?:-C\d+)?(?:-T\d+)?|N\d+|C\d+)\s+(?:\d+\s+)?)?"
    r"(?P<value>\d[\d,.'  ]*|<not counted>|<not supported>)\s+"
    r"(?:(?P<unit>msec|ms|ns|us|Joules|MiB|GiB|MB|GB)\s+)?"
    r"(?P<event>[A-Za-z_][\w.:/=,-]*/?\w*)"
    r"(?P<rest>.*)$"
)
RUNNING = re.compile(r"\(\s*(\d+(?:\.\d+)?)%\s*\)\s*$")
TMA_LINE = re.compile(r"#\s*(\d+(?:\.\d+)?)\s*%\s*(tma_)?(\w+)")
TOPDOWN_GROUP = re.compile(r"^\s*(TopdownL\d|PipelineL\d)\s*(?:\((\w+)\))?", re.I)
ELAPSED = re.compile(r"^\s*([\d.,]+)\s+seconds\s+time\s+elapsed")
HEADER = re.compile(r"Performance counter stats for\s+(.+?):?\s*$")


def _record_tma(a: Analysis, name: str, pct: float, origin: str, intel_pcore: bool, pmu: str = "") -> None:
    canon = TMA_NAMES.get(name.lower().replace("tma_", "").replace("_", " "), None) or TMA_NAMES.get(name.lower())
    if canon is None:
        return
    frac = pct / 100.0
    label = f"{canon} (level 1)" + (f" [{pmu}]" if pmu else "")
    m = Metric(name=label, value=round(frac, 4), unit="of slots", formula=origin, inputs=[origin])
    if intel_pcore and canon in TMA_THRESHOLDS:
        text, limit = TMA_THRESHOLDS[canon]
        m.source = TMA_SOURCE
        if frac > limit:
            m.flag = f"above Intel's threshold {text}"
            a.flagged.append((frac / limit, TMA_ROUTING[canon]))
    if all(x.name != label for x in a.metrics):
        a.metrics.append(m)


def parse_perf_plain(lines: list[str], a: Analysis) -> bool:
    found = False
    td_header: list[str] | None = None
    td_rows: list[list[float]] = []
    group_pmu = ""
    group_kind = ""
    in_block = False
    for line in lines:
        if not line.strip():
            continue
        h = HEADER.search(line)
        if h:
            in_block, found = True, True
            a.notes.append(f"perf stat of {h.group(1).strip()}")
            continue
        m = ELAPSED.match(line)
        if m:
            a.elapsed = parse_number(m.group(1), integer=False)
            continue
        if re.match(r"^\s*[\d.,]+\s+seconds\s+(user|sys)", line):
            continue
        low = line.lower()
        # old --topdown: a header of the four level 1 names, then rows of percentages
        if "retiring" in low and "frontend bound" in low and "%" not in low:
            td_header = [n for n in ("retiring", "bad speculation", "frontend bound", "backend bound") if n in low]
            td_header.sort(key=low.index)
            found = in_block = True
            continue
        if td_header is not None and "%" in line and not PLAIN.match(line):
            vals = [float(x) for x in re.findall(r"(\d+(?:\.\d+)?)%", line)]
            if len(vals) >= len(td_header):
                td_rows.append(vals[: len(td_header)])
                continue
        g = TOPDOWN_GROUP.search(line)
        if g:
            group_kind, group_pmu = g.group(1), (g.group(2) or "")
            found = True
        tma = TMA_LINE.search(line)
        if tma and (group_kind or tma.group(2)):
            intel = bool(tma.group(2)) and group_pmu != "cpu_atom" and not group_kind.lower().startswith("pipeline")
            _record_tma(a, tma.group(3), float(tma.group(1)), f"perf {group_kind or 'tma'}", intel, group_pmu)
            found = True
            if not PLAIN.match(line):
                continue
        if line.lstrip().startswith("#"):
            continue
        pm = PLAIN.match(line)
        if not pm:
            if in_block and not g and not line.lstrip().startswith(("Performance", "Some events")):
                a.unparsed.append(line.strip()[:200])
            continue
        raw = pm.group("event").rstrip(",")
        key, pmu, mods = split_event(raw)
        c = Count(event=key, raw=raw, pmu=pmu, mods=mods, unit=pm.group("unit") or "")
        v = pm.group("value")
        if v.startswith("<"):
            c.status = v.strip("<>")
        else:
            c.value = parse_number(v, integer=not c.unit)
        r = RUNNING.search(pm.group("rest"))
        if r:
            c.running = float(r.group(1))
        a.counts.append(c)
        found = True
    if td_header and td_rows:
        avg = [sum(r[i] for r in td_rows) / len(td_rows) for i in range(len(td_header))]
        for name, pct in zip(td_header, avg):
            _record_tma(a, name, pct, "perf stat --topdown" + (f", mean of {len(td_rows)} rows" if len(td_rows) > 1 else ""), True)
    if found and "perf stat" not in a.kinds:
        a.kinds.append("perf stat")
    return found


def parse_perf_csv(lines: list[str], a: Analysis) -> bool:
    sep = None
    for cand in (",", ";", "\t"):
        hits = sum(1 for ln in lines if ln.count(cand) >= 3 and re.search(r"[A-Za-z]", ln))
        if hits >= max(1, len(lines) // 3):
            sep = cand
            break
    if sep is None:
        return False
    found = False
    for line in lines:
        if not line.strip() or line.startswith("#"):
            continue
        fields = _rejoin_event(line.split(sep), sep)
        for i in range(len(fields) - 2):
            v = fields[i].strip()
            ev = fields[i + 2].strip()
            if (re.fullmatch(r"[\d.]+", v) or v in ("<not counted>", "<not supported>")) and re.search(r"[A-Za-z]", ev):
                key, pmu, mods = split_event(ev)
                c = Count(event=key, raw=ev, pmu=pmu, mods=mods, unit=fields[i + 1].strip())
                if v.startswith("<"):
                    c.status = v.strip("<>")
                else:
                    c.value = float(v)
                if len(fields) > i + 4 and re.fullmatch(r"[\d.]+", fields[i + 4].strip()):
                    c.running = float(fields[i + 4])
                a.counts.append(c)
                found = True
                break
    if found:
        a.kinds.append("perf stat -x")
    return found


def _rejoin_event(fields: list[str], sep: str) -> list[str]:
    """'cpu/event=0x3c,umask=0x0/' is split by a comma separator: put it back."""
    out: list[str] = []
    buf: str | None = None
    for f in fields:
        if buf is not None:
            buf += sep + f
            if f.endswith("/") or re.search(r"/\w*$", f):
                out.append(buf)
                buf = None
            continue
        if re.match(r"^[\w.-]+/[^/]*$", f) and not f.endswith("/"):
            buf = f
            continue
        out.append(f)
    if buf is not None:
        out.append(buf)
    return out


def parse_perf_json(lines: list[str], a: Analysis) -> bool:
    found = False
    for line in lines:
        s = line.strip()
        if not (s.startswith("{") and s.endswith("}")):
            continue
        try:
            obj = json.loads(s)
        except ValueError:
            continue
        if "event" not in obj or "counter-value" not in obj:
            continue
        key, pmu, mods = split_event(str(obj["event"]))
        c = Count(event=key, raw=str(obj["event"]), pmu=pmu, mods=mods, unit=str(obj.get("unit") or ""))
        val = str(obj.get("counter-value", ""))
        if val.startswith("<"):
            c.status = val.strip("<>")
        else:
            try:
                c.value = float(val)
            except ValueError:
                continue
        if obj.get("pcnt-running") is not None:
            c.running = float(obj["pcnt-running"])
        a.counts.append(c)
        found = True
    if found:
        a.kinds.append("perf stat -j")
    return found


# ----- toplev --------------------------------------------------------------------------

TOPLEV = re.compile(
    r"^\s*(?:(?P<where>[SCN]\d[\w-]*)\s+)?(?P<area>FE|BAD|BE|RET|BE/Mem|BE/Core|FE/\w+|Info\.\w+)\s+"
    r"(?P<node>[A-Z][\w.]*)\s+(?P<unit>%\s*\w+|\w+)\s+(?P<value>\d+(?:\.\d+)?)(?P<rest>.*)$"
)


def parse_toplev(lines: list[str], a: Analysis) -> bool:
    found = False
    level1: dict[str, list[float]] = {}
    for line in lines:
        m = TOPLEV.match(line)
        if not m:
            continue
        found = True
        node, value, rest = m.group("node"), float(m.group("value")), m.group("rest")
        unit = m.group("unit")
        if "." not in node and node in TMA_THRESHOLDS and "%" in unit:
            level1.setdefault(node, []).append(value)
        elif "%" in unit:
            a.metrics.append(Metric(name=node, value=round(value / 100.0, 4), unit="of slots", formula="toplev", inputs=["toplev"]))
        if "<==" in rest:
            a.notes.append(f"toplev marks {node} as the bottleneck")
            a.routing.append(node.replace("_", " ").replace(".", " ").lower())
        mux = re.search(r"\[\s*(\d+(?:\.\d+)?)%\]", rest)
        if mux and float(mux.group(1)) < 100:
            a.counts.append(Count(event=node, raw=node, running=float(mux.group(1))))
    for node, vals in level1.items():
        _record_tma(a, node, sum(vals) / len(vals), "toplev" + (f", mean of {len(vals)} rows" if len(vals) > 1 else ""), True)
    if found:
        a.kinds.append("toplev")
    return found


# ----- compiler remarks ------------------------------------------------------------------

REMARK = re.compile(r"(?P<loc>[^\s:]+:\d+(?::\d+)?):\s*(?P<kind>missed|optimized|note|remark|warning):\s*(?P<msg>.+?)\s*$")


def parse_remarks(lines: list[str], a: Analysis) -> bool:
    found = False
    reasons: list[str] = []
    for line in lines:
        m = REMARK.search(line)
        if not m:
            continue
        msg = m.group("msg")
        low = msg.lower()
        if "vectoriz" not in low and "vectorise" not in low and "loop" not in low:
            continue
        found = True
        flag = re.search(r"\[-R(pass|pass-missed|pass-analysis)=([\w-]+)\]", msg)
        text = re.sub(r"\s*\[-R[\w-]+=[\w-]+\]", "", msg).strip().rstrip(".")
        success = m.group("kind") == "optimized" or "vectorized loop" in low or (flag and flag.group(1) == "pass")
        if success:
            a.remarks.append(f"{m.group('loc')}: {text}")
            continue
        a.remarks.append(f"{m.group('loc')}: {text}")
        for pattern, words in REMARK_REASONS:
            if re.search(pattern, low) and words not in reasons:
                reasons.append(words)
    if found:
        a.kinds.append("compiler remarks")
        a.routing.append("auto-vectorization vectorizer remarks")
        a.routing.extend(reasons)
        missed = sum(1 for r in a.remarks if "not vectorized" in r.lower())
        if missed:
            a.notes.append(f"{missed} loop(s) not vectorised; the reasons are in the remarks")
    return found


# ----- assembly and code -----------------------------------------------------------------

ASM_LINE = re.compile(
    r"^\s*(?:[0-9a-f]+:\s+(?:[0-9a-f]{2}\s)+\s*|\d+\.\d+\s*[│|:]\s*|[│|]\s*)?"
    r"(?P<mn>(?:lock\s+)?[a-z][a-z0-9]{1,15}(?:\.[a-z0-9]+)?)\s+(?P<ops>[%$\w\[\](){},.:+*#-][^;]*)$"
)
REGISTER = re.compile(r"%?\b([xyz]mm\d+|[re][a-ds][xi]|r\d+[dwb]?|[vqdsbh]\d+|[xw]\d+)\b")


def parse_asm(lines: list[str], a: Analysis) -> bool:
    mnemonics: dict[str, int] = {}
    n_lines = 0
    for line in lines:
        m = ASM_LINE.match(line)
        if not m or not REGISTER.search(m.group("ops")):
            continue
        n_lines += 1
        mn = m.group("mn")
        if mn.startswith("lock "):
            mn = mn[5:]
            if "atomic contention" not in a.routing:
                a.routing.append("atomic contention")
        mnemonics[mn] = mnemonics.get(mn, 0) + 1
    if n_lines < 3:
        return False
    a.kinds.append("assembly")
    text = "\n".join(lines)
    if re.search(r"\bzmm\d+", text):
        a.routing.append("avx512")
    elif re.search(r"\bymm\d+", text):
        a.routing.append("avx2")
    if re.search(r"\b[zp]\d+\.[bhsd]\b|\bwhilelo\b|\bptrue\b", text):
        a.routing.append("sve")
    for mn in sorted(mnemonics, key=lambda k: -mnemonics[k])[:12]:
        if mn not in a.terms:
            a.terms.append(mn)
        for pattern, words in ASM_FAMILIES:
            if re.search(pattern, mn) and words not in a.routing:
                a.routing.append(words)
    return True


def parse_code(text: str, a: Analysis) -> bool:
    if not re.search(r"[;{}]\s*$|#include|#pragma|\bfn\b|\bdef\b|\bfor\s*\(", text, re.M):
        return False
    found = False
    for pattern, words in CODE_PATTERNS:
        hits = re.findall(pattern, text)
        if hits:
            found = True
            if words not in a.routing:
                a.routing.append(words)
            for h in re.findall(r"\b_mm\d*_\w+", text)[:6] if "intrinsics" in words else []:
                if h not in a.terms:
                    a.terms.append(h)
    if found:
        a.kinds.append("code")
    return found


# ----- metrics ---------------------------------------------------------------------------


def _totals(a: Analysis) -> dict[tuple[str, str, str], float]:
    """Counts summed per (pmu, event, modifiers): per-CPU and interval lines add up."""
    out: dict[tuple[str, str, str], float] = {}
    for c in a.counts:
        if c.value is not None and c.status == "ok":
            k = (c.pmu, c.event, c.mods)
            out[k] = out.get(k, 0.0) + c.value
    return out


def compute_metrics(a: Analysis) -> None:
    totals = _totals(a)
    groups = {(p, m) for (p, _, m) in totals}
    generic: list[str] = []  # topics of the plain ratios; used only without top-down data

    def get(pmu: str, mods: str, ev: str) -> float | None:
        return totals.get((pmu, ev, mods))

    for pmu, mods in sorted(groups):
        tag = "/".join(x for x in (pmu, mods) if x)
        sfx = f" [{tag}]" if tag else ""

        def ratio(name, num, den, scale=1.0, unit="", formula=""):
            n, d = get(pmu, mods, num), get(pmu, mods, den)
            if n is None or not d:
                return
            a.metrics.append(
                Metric(name=name + sfx, value=round(n / d * scale, 4), unit=unit, formula=formula or f"{num} / {den}", inputs=[num, den])
            )
            if name in ROUTING and ROUTING[name] not in generic:
                generic.append(ROUTING[name])

        ratio("IPC", "instructions", "cycles", formula="instructions / cycles")
        ratio("branch miss rate", "branch-misses", "branches", 100, "%")
        ratio("branch MPKI", "branch-misses", "instructions", 1000, "per 1k instructions")
        ratio("L1d miss rate", "L1-dcache-load-misses", "L1-dcache-loads", 100, "%")
        ratio("L1d MPKI", "L1-dcache-load-misses", "instructions", 1000, "per 1k instructions")
        ratio("LLC miss rate", "LLC-load-misses", "LLC-loads", 100, "%")
        ratio("LLC MPKI", "LLC-load-misses", "instructions", 1000, "per 1k instructions")
        ratio("cache miss rate", "cache-misses", "cache-references", 100, "%")
        ratio("dTLB miss rate", "dTLB-load-misses", "dTLB-loads", 100, "%")
        ratio("iTLB miss rate", "iTLB-load-misses", "iTLB-loads", 100, "%")
        ratio("frontend stall share", "stalled-cycles-frontend", "cycles", 100, "% of cycles")
        ratio("backend stall share", "stalled-cycles-backend", "cycles", 100, "% of cycles")
        slots = get(pmu, mods, "slots")
        if slots:
            for ev, name in (
                ("topdown-retiring", "Retiring"),
                ("topdown-bad-spec", "Bad_Speculation"),
                ("topdown-fe-bound", "Frontend_Bound"),
                ("topdown-be-bound", "Backend_Bound"),
            ):
                v = get(pmu, mods, ev)
                if v is not None:
                    _record_tma(a, name, 100.0 * v / slots, f"{ev} / slots", pmu != "cpu_atom", pmu)
    if not any("level 1" in m.name for m in a.metrics):
        a.routing.extend(g for g in generic[:3] if g not in a.routing)
    if a.elapsed:
        for (pmu, ev, mods), v in totals.items():
            if ev == "task-clock":
                a.metrics.append(
                    Metric(name="CPUs utilised", value=round(v / 1000.0 / a.elapsed, 3), formula="task-clock (ms) / 1000 / elapsed (s)", inputs=["task-clock", "elapsed"])
                )
            if ev == "context-switches":
                a.metrics.append(
                    Metric(name="context switches per second", value=round(v / a.elapsed, 1), unit="/s", formula="context-switches / elapsed", inputs=["context-switches", "elapsed"])
                )


def trust_notes(a: Analysis) -> None:
    running = [c.running for c in a.counts if c.running is not None and c.status == "ok"]
    if running and min(running) < 99.99:
        a.notes.append(
            f"Counters were multiplexed (lowest running share {min(running):.1f}%): perf scaled them up, so ratios "
            "between events counted at different times carry error, worst on short or phase-changing runs."
        )
        a.routing.append("multiplexing counters")
    for status in ("not counted", "not supported"):
        names = sorted({c.raw for c in a.counts if c.status == status})
        if not names:
            continue
        why = (
            "never scheduled (more events than counters, or the run ended first)"
            if status == "not counted"
            else "not available on this CPU or kernel (common on virtual machines and macOS)"
        )
        a.notes.append(f"{', '.join(names[:6])}: {status}, {why}; metrics needing them are left out.")
    pmus = {c.pmu for c in a.counts if c.pmu}
    if {"cpu_core", "cpu_atom"} <= pmus:
        a.notes.append("Hybrid CPU: P-core (cpu_core) and E-core (cpu_atom) counts are kept apart and never divided by each other.")
    # identifiers for exact search: the raw events actually printed
    for c in a.counts:
        name = c.raw.split("/")[1] if c.raw.count("/") >= 2 else c.raw.split(":")[0]
        if ("." in name or "_" in name) and name not in a.terms and len(a.terms) < 16:
            a.terms.append(name)


def analyse(text: str) -> Analysis:
    a = Analysis()
    if len(text) > MAX_CHARS:
        a.notes.append(f"Only the first {MAX_CHARS} characters were read.")
        text = text[:MAX_CHARS]
    lines = text.replace("\r\n", "\n").split("\n")[:MAX_LINES]
    if not parse_perf_json(lines, a):
        if not parse_perf_plain(lines, a):
            parse_perf_csv(lines, a)
    parse_toplev(lines, a)
    parse_remarks(lines, a)
    if not parse_asm(lines, a):
        parse_code(text, a)
    compute_metrics(a)
    trust_notes(a)
    if a.counts and "perf stat" in " ".join(a.kinds) and "top-down" not in a.routing and not any("level 1" in m.name for m in a.metrics):
        a.routing.append("perf stat counters")
    # the level furthest over its threshold leads the search, whatever order perf printed them in
    for _, topic in sorted(a.flagged, key=lambda x: -x[0]):
        if topic not in a.routing:
            a.routing.insert(len([r for r in a.routing if r in {t for _, t in a.flagged}]), topic)
    if any("level 1" in m.name for m in a.metrics):
        a.routing.insert(0, "top-down")
        a.topics.append("top-down analysis")
        if any(m.source for m in a.metrics):
            a.routing.insert(1, "intel tma metrics")
        elif any("PipelineL" in m.formula for m in a.metrics):
            a.routing.insert(1, "amd zen pipeline")
    elif any(k.startswith("perf stat") for k in a.kinds) and a.counts:
        a.notes.append(
            "No top-down data in this output. The list's method classifies the bottleneck next: perf stat --topdown "
            "or -M TopdownL1 (Intel), -M PipelineL1 (AMD Zen 4 and later), or toplev give the level 1 split."
        )
        a.topics.append("top-down analysis")
    if any(n.startswith("Counters were multiplexed") or ": not supported" in n for n in a.notes):
        a.topics.append("counters, events")
    if "compiler remarks" in a.kinds:
        a.topics.append("auto-vectorisation")
    if "atomics memory ordering" in a.routing or "atomic contention" in a.routing:
        a.topics += ["memory models and atomics", "cache-line contention"]
    if any(r in a.routing for r in ("gather", "avx512", "avx2", "sve", "intrinsics simd")):
        a.topics.append("simd instruction sets")
    if "locks contention" in a.routing or "allocator malloc" in a.routing:
        a.topics.append("locks, contention")
    seen: list[str] = []
    for r in a.routing:
        if r not in seen:
            seen.append(r)
    a.routing = seen
    if not a.kinds:
        a.notes.append("No tool output was recognised; the text was used as extra search words.")
        a.terms = [w for w in re.findall(r"[A-Za-z_][\w.]{3,}", text)[:12]]
    return a
