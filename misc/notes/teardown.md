# Teardown of the reference list

Reference: `wafer-ai/gpu-perf-engineering-resources` at `1c52412` (read 2026-09-15).
Files read: `README.md`, `CONTRIBUTING.md`, `scripts/check_guide.py`,
`.github/workflows/links.yml`, `.github/workflows/quality.yml`,
`.github/pull_request_template.md`, the three issue templates, and the git
history back to the V1 to V2 rewrite (`2f1f12c`).

This note records structure and conventions only. No entry, annotation or
sentence from the reference is reused. The reference is about GPUs; this
list is about CPUs.

## What the reference is

A single README that is a reading path first and a reference second. The
README is the whole product: there is no site, no generated index, no
per-topic pages. Everything else in the repo exists to keep the README
honest (a structure checker, a link checker, contribution rules).

## Ordering logic

The list follows one unit of work through the machine, then widens:

1. an unnumbered **Start here** block, 8 items, explicitly ordered, to be
   read top to bottom by a newcomer;
2. numbered sections that go **down** the stack (programming model, then
   machine code, then kernels, then tools) and then **out** (engines, then
   distributed systems, then current hardware);
3. a dated **Frontier** block that holds things whose evidence is still
   moving;
4. a **Source policy** block that restates the admission rule.

Sections are H2, subsections are H3. Every H3 is a small cluster of three
to five links. There are no H4s and no tiers. The V1 of the repo had
"Tier 1 / Tier 2 / Tier 3" under every H3 and a "how to read" preamble;
V2 removed both and the checker now rejects any heading that starts with
"Tier" and any "**Checkpoint:**" marker. Ordering inside a section is the
dependency order, not importance and not date.

The CPU list keeps the same shape: Start here, then one instruction end to
end, then the memory hierarchy, then how to measure, then models, then
single-thread throughput, then compilers, then concurrency, then the
kernel boundary, then tail latency, then CPU inference, then hardware and
benchmark suites, then a dated watchlist, then the source policy.

## Annotation style

One entry is exactly one line:

    - [Title](URL) - Annotation.

- Hyphen with a space on each side separates link from annotation.
- The annotation is a fragment or a single short sentence. It names the
  mechanism the source teaches or the thing it defines. It does not
  summarise the source, does not name the author (V1 did; V2 dropped it),
  does not carry emoji, dates, or "recently added" markers.
- Titles are the source's own title, shortened where long. Section names
  are sentence case.
- Numbered lists are used only in Start here, where the order is the
  point. Everywhere else bullets.
- Section preambles are one or two sentences and appear only where a
  reader needs a warning (for example the hardware section opens by saying
  vendor peak numbers are not measurements).

The CPU list keeps the one-line form and the hyphen separator, and adds
one rule the reference does not have: the annotation must say **why the
entry earns its place**, not what the entry is. "The normative X
reference" is a description; "the only public per-instruction latency
table that is measured rather than quoted" is a reason.

## Exclusions and why

Dropped between V1 and V2 of the reference, and the reason each stays out
of the CPU list:

- **Industry analysis, practitioner blogs, communities** (Discord links,
  lecture series as sections). They are not primary and they rot. A blog
  can enter only as a specific post that is a direct implementer report.
- **Tiers and reading-order preambles.** The order of the list is the
  reading order; a second ordering axis confuses it.
- **Author bylines in annotations.** They cost width and invite
  name-dropping instead of reasons.
- **Leaderboards and survey papers.** Surveys are secondary by definition;
  leaderboards move.
- **Marketing pages and vendor blog posts** that are not the canonical
  location of a mechanism's definition.
- **Duplicate links within a section.** The checker rejects them. The
  same URL may appear in two different sections if it belongs in both.

The CPU list adds: no aggregators, no listicles, no SEO summaries, no
AI-generated explainers, no third-party mirrors of a document that has an
official home, no paywalled-only copies where an author's copy exists.

## Evidence standard

The reference requires five fields before a performance number may be
stated: hardware and software versions, workload, precision and
algorithm, baseline, correctness method. If any is missing the number is
omitted (not softened, not caveated: omitted).

The CPU list requires seven, because a CPU number is meaningless without
the parts of the machine that vary run to run:

1. CPU model and microarchitecture
2. core count used
3. frequency, and whether turbo and SMT were on
4. compiler and flags
5. workload
6. baseline
7. method (how measured, how many runs, what statistic)

Any number missing any field goes to the watchlist or is cut. This applies
to numbers in annotations and to numbers in `misc/benchmarks/`.

## Watchlist versus core

The reference keeps a dated Frontier block outside the numbered core. Its
entries are one line each, not links, and each states the condition that
would promote it ("pending shipped systems and reproducible
measurements"). The block carries a "Verified on" date. Items enter the
core only when three things exist: a specification or original paper, a
shipped implementation, and a reproducible measurement.

The CPU list keeps this split under the name **Watchlist**, keeps the
date, keeps the promotion condition on every line, and adds one stricter
rule: a claim that fails the seven-field test lands here rather than in
the core, even if the mechanism itself is real.

## Tooling conventions

- `scripts/check_guide.py` (reference): checks internal anchors resolve,
  no duplicate URL within an H3, every H3 has at least one external link,
  no tier headings, no checkpoints. Runs on push to main and on PRs.
- External links are checked separately, weekly and on demand, with a
  third-party action that accepts 403 and 429 as live because some primary
  sources block bots. No issue is opened on failure.
- Issue templates for add, broken link, quality concern. The PR template
  asks the four admission questions and the evidence fields.
- The README says "MIT" but the repo has no `LICENSE` file.

The CPU list keeps the two-check split (structure on every push, external
links weekly) but writes its own link checker rather than taking a
dependency, runs the link check on push as well as weekly, opens an issue
on failure, ships a real `LICENSE`, and adds a format linter so that every
entry line matches the one-line form above.

## What is not carried over

- Cover image.
- Hiring line.
- Any GPU content, term, or entry.
- The Frontier sub-block on AI-generated kernels (no CPU equivalent with a
  hardened evaluator yet; the closest CPU items go to the watchlist with
  their promotion condition).
