# Contributing

This is a reading path, not a link collection. A new entry has to make the
path clearer or more complete for someone reading the README top to bottom,
and it has to pass the rules below. Pull requests that do not answer the
questions in the template are closed without review.

## The rules

These are the curation rules, verbatim, as set by the maintainer. They are
not negotiable and they apply to every line of the README.

> - Primary sources only: original papers, vendor optimization manuals and ISA
>   references, creator repos, kernel and compiler documentation. No blogspam, no
>   listicles, no SEO aggregators, no AI-generated posts.
> - Every entry gets one line saying why it earns a place, not what it is.
> - Every performance claim carries: CPU model and microarchitecture, core count,
>   frequency and turbo/SMT state, compiler and flags, workload, baseline, and
>   measurement method. Missing any of those means watchlist or cut. Stricter than the
>   reference, because CPU numbers are easy to get wrong by accident.
> - Every URL resolves to the live canonical source. No dead links, no third-party PDF
>   mirrors where an official one exists.
> - /benchmarks/ — for at least one claim per major section, commit a reproducible
>   microbenchmark I can run myself: source, build script, the machine it ran on, raw
>   numbers, and the analysis. This directory is the part of the repo nobody can fork
>   their way into and it is the whole point.

## What "primary" means here

One of:

- the paper that introduced the mechanism, at the publisher, on arXiv, or
  on the author's own page;
- the specification, ISA reference, optimisation manual, or official
  documentation that defines it, in the vendor's or project's own document
  library;
- the repository that implements it;
- a direct implementer report: the person who did the work, with code,
  measurements that carry all seven fields, and enough detail to reproduce.

Not primary: summaries, explainers, tutorials, surveys, slide decks without
a paper, marketing pages, press coverage, Wikipedia, Q&A sites, talks that
are not the official upload, and any post that restates a mechanism someone
else established. If a more primary source exists for the same mechanism,
the entry uses that one.

## Entry format

Exactly one line:

    - [Title](URL) - Why it earns its place.

The reason says what the source uniquely establishes, measures, defines or
proves. It does not summarise, does not name the author, does not carry
numbers, and stays under 160 characters. No em dashes. British spelling in
prose except `quantization`; titles keep their own spelling. The Start here
list is numbered; every other section uses bullets. Watchlist lines state
the condition that would promote the item into the core.

`misc/scripts/check_format.py` enforces the mechanical parts of this and runs in
CI on every push.

## Performance numbers

A number may appear in the README or in `misc/benchmarks/` only when its source
states all seven fields:

1. CPU model and microarchitecture
2. core count used
3. frequency, and whether turbo (or DVFS) and SMT were on
4. compiler and flags
5. workload
6. baseline
7. measurement method (how timed, how many runs, which statistic)

If any field is missing, the number is left out. The entry may stay if it
earns its place without the number; otherwise it goes to the watchlist with
the missing fields named, or it is cut.

## Benchmarks

A benchmark directory contains `bench.c`, `build.sh`, `run.sh`, a README
with the claim, the method, the seven fields, the results table and the
analysis, and the committed `results/raw.txt` and `results/summary.md`
from the machine named in the README. `QUICK=1 ./run.sh` must finish in
under ten seconds so CI can smoke-test it. See `misc/benchmarks/README.md`.

## Before opening a pull request

Answer four questions in the PR description:

1. Which mechanism does this source establish, define, measure or prove?
2. Why is it primary, and why is this URL the canonical home?
3. Where does it go in the dependency order, and why there?
4. Which existing entry does it replace, if the subsection already has six?

Then run the checks locally:

    python3 misc/scripts/check_format.py
    python3 misc/scripts/check_links.py

Both must pass. `check_links.py` reports 403 and 429 as "blocked" rather
than dead because some vendor libraries refuse automated clients; if your
URL is blocked, say in the PR that you opened it in a browser.

Disclose any conflict of interest (you wrote it, your employer sells it).

## Watchlist entries

Anything without a specification or original paper, a shipped
implementation, and a seven-field measurement stays on the watchlist. State
the promotion condition on the line.

## Tone

Discuss the source, not the person proposing it. Be direct.
