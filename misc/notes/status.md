# Status

Written 2026-09-15 at the end of the build. Repository:
https://github.com/usamahz/cpu-perf-engineering-resources (`main`, private).

## What shipped

- `README.md`: Start here, 14 numbered sections and the Frontier, 304 entries over 290 unique URLs, 9,500
  words, 694 lines. Section sizes: Start here 10, then sections 1 to 14 at 18, 19, 21, 20, 20,
  21, 21, 20, 15, 20, 20, 30 (Inference on CPU, six subsections), 20, 15,
  Frontier 14. The skeleton matches the reference list: cover image, four-line
  intro, Contents, an unnumbered Start here with a practical-companion line,
  numbered core sections with H3 subsections, a dated Frontier, Source policy,
  License, Maintainer. Every subsection has three to six entries, every reason is
  one line under 120 characters, every URL appears in at most three places
  (the Start here path re-lists its entries in their home sections; the
  Intel and Arm reference manuals and the AMD Zen 5 guide appear where a
  different chapter is the point).
- `CONTRIBUTING.md` with the five curation rules quoted verbatim (the one
  em dash in the repo is inside that quotation), the definition of primary,
  the entry format, the seven fields, the four PR questions.
- `LICENSE` (MIT).
- `misc/scripts/check_links.py`: standard library only; HEAD then GET, three
  retries with a longer timeout each time, tries IPv4 before IPv6 (GitHub
  runners have no IPv6 route), follows redirects, and reports `dead`
  (404, 410, another non-block 4xx, or a host that no longer resolves;
  fails the build), `unreachable` (timeouts, resets, unreachable routes,
  persistent 5xx; listed in the job summary but not counted as dead unless
  `--fail-unreachable`), `moved` (permanent redirect to another document),
  and `blocked` (401, 403, 429, or a server whose TLS chain is incomplete
  but serves the page). Final run
  on `README.md`, `CONTRIBUTING.md`, `misc/benchmarks/README.md`: ok 251, moved 0,
  blocked 39, dead 0. Every blocked URL was opened in a browser during the
  build and serves the document: Intel's document library, Arm's
  `support.arm.com` viewer, ACM DL and ACM Queue (Cloudflare challenge; each
  DOI also resolves through doi.org), Red Hat docs, SourceForge, INFORMS,
  and `lemire.me`; `agner.org` rate-limits the parallel checker to 429 but
  serves every PDF to a single request.
- `misc/scripts/check_format.py`: the entry-line grammar, reason length, full
  stop, Start here numbering, watchlist promotion condition, no em dashes
  or placeholder text, internal anchors, Contents completeness, relative
  link targets, no duplicate URL inside a subsection, no empty subsection.
- `misc/scripts/build_changelog.py` generates `misc/notes/changelog.md`.
- `.github/workflows/quality.yml` (push and PR): format lint, script
  compile, every benchmark compiled on Linux x86-64, every benchmark built
  and smoke-run with `QUICK=1` on macOS arm64.
- `.github/workflows/links.yml` (push, PR, weekly Monday cron, manual):
  runs the link checker over the README, CONTRIBUTING and all benchmark
  READMEs; on failure opens or updates a `dead-links` issue with the
  report, and closes it when a later run is clean.
- Issue templates (add an entry, dead or moved link, evidence concern) and
  a PR template with the four questions and the seven fields.
- `misc/benchmarks/`: fourteen microbenchmarks, one per numbered section from
  1 to 14, each with `bench.c`, `build.sh`, `run.sh`, a README carrying the
  claim, method, the seven fields, the results table and the analysis, and
  the committed `results/raw.txt` and `results/summary.md` from the final
  serial run. `run_all.sh` and `compile_all.sh` drive them. Headline
  results from the committed run on the Apple M4 Pro:
  - 02: a data-dependent branch over unsorted data costs 10.6 times the
    sorted case; about 19 cycles per mispredict.
  - 03: eight independent accumulators run 5.7 times one dependency chain;
    sixteen reach 3.9 adds per cycle against a plateau of 4.
  - 04: an L2 hit is 8.9 times an L1 hit, memory 20.8 times L2; spreading
    the same lines over one page each doubles the cost.
  - 05: an unused result measures 0 ns; one run per side leaves a fifth of
    paired runs disagreeing by more than 2 percent.
  - 06: a 10 flop per byte kernel reaches 0.96 of the FMA peak on one
    core; saxpy lands within 3 percent of its bandwidth-roof prediction.
  - 07: the array-of-structs sum takes 14 times the structure-of-arrays
    sum, on a 16 to 1 byte ratio.
  - 08: with possible aliasing the loop runs the scalar fallback at 5.3
    times the vectorised time in L1; `restrict` restores it.
  - 09: adjacent atomic counters cost 135 times padded ones at 8 threads.
  - 10: first touch costs 669 ns per 16 KiB page against 4 ns for the
    third pass.
  - 11: a 4 KiB `pread` costs 1.12 times a 1-byte one; the fixed cost is
    about 280 ns.
  - 12: the open-loop p99 is 1951 times the closed-loop p99 on one
    identical server timeline.
  - 13: the vendor BLAS is 624 times the naive loop; a NEON microkernel
    reaches 44 times naive and 87 percent of the measured FMA peak, and the
    remaining 14 times is the matrix unit Accelerate uses.
  - 14: the same binary under background QoS runs the FMA loop 12 times
    slower, the streaming sum 10 times, the add chain 3 to 4 times.
  - 15: one thread reaches 0.44 of Apple's 273 GB/s figure; the DRAM
    plateau is 0.83 at 8 threads; cache-resident triad reads 2.9 times the
    vendor figure.
- `misc/cover.avif`: Hokusai's Great Wave (Met open access, CC0) rendered as a
  hex-glyph grid, chosen by a three-judge panel over three other treatments;
  renderer and provenance in `misc/notes/banner/`.
- `misc/notes/teardown.md` (the reference's conventions), `misc/notes/owner-brief.md`,
  `misc/notes/benchmark-brief.md` and `misc/notes/voice.md` (the briefs the section
  owners, benchmark authors and editor worked from), `misc/notes/sections/`
  (the sixteen section drafts with their Rejected, Claims and Link notes
  blocks), and `misc/notes/changelog.md` (generated: 304 included, 541 rejected
  with the rule each failed, 171 performance numbers examined against the
  seven-field rule with a verdict).

## How it was built

Sixteen section owners drafted in parallel from their named corpora; each
draft was reviewed by a combined link, primary-source and evidence
reviewer and fixed; a red team read the assembled README cold against the
GPU reference section by section and a whole-list cold reader judged
ordering, intro and length; the gaps were applied; one editor set one
voice across all sixteen files; a trim pass brought the list from 399
entries to 304 with one home per URL and shorter reasons, after the cold
reader judged 399 entries at 22 words each too long for one sitting; four
corpus items the trim had dropped (Mike Acton, simdjson, Chandler
Carruth, Justine Tunney) were restored by hand. Fourteen benchmark authors
worked in parallel; each benchmark was refuted by a sceptic reading its
assembly and repaired; all fourteen were then re-run serially and every
README reconciled against the new table.

## What was rejected, and why

The full list is `misc/notes/changelog.md`. The main classes:

- Secondary sources: Wikipedia, explainers, tutorials, surveys, press
  coverage, StackOverflow, blog posts restating someone else's mechanism
  (rule 1). Examples: the ACM Queue compiler-optimisation tutorial, the
  Data-Oriented Design online book, the "latency numbers every programmer
  should know" gist, the VTune cookbook restating the top-down paper.
- Mirrors and paywalled copies where a canonical home exists (rule 4):
  LWN and FreeBSD serialisations of Drepper, IEEE Xplore and Semantic
  Scholar copies of papers with author pages, the Berkeley tech report
  draft of the Roofline paper.
- Dead or withdrawn: Intel Architecture Code Analyzer, the Intel rdtsc
  white paper, the Intel prefetcher-control page, the Neoverse V1 guide's
  old document id.
- Vendor performance multiples: every "X times faster" from Intel, AMD,
  Arm, AWS and Microsoft fails the seven-field rule; the parts sit in
  section 14 by what they changed structurally, and the unmeasured ones on
  the watchlist.
- Numbers from otherwise primary sources that lack a field: BOLT's and
  AutoFDO's headline percentages (no turbo or SMT state, no run count),
  Justine Tunney's llamafile figures (no core count or compiler flags),
  T-MAC and BitNet speedups (no frequency, compiler or flags), Travis
  Downs' AVX-512 transition timings (model stated, compiler not). The
  entries stay where the mechanism stands without the number; the numbers
  are recorded under Claims with the missing fields named.
- Trimmed for length after the cold read: second sources for a mechanism
  already established (Hoard and TCMalloc stay, jemalloc and mimalloc go;
  one flame-graph entry rather than three; the nanoBench paper rather than
  paper plus repository), repositories whose reason only said where code
  lives, and repeated citations of the same manual across sections. Each
  such line names the entry that now carries the mechanism.

## Known limits

- All benchmarks were measured on one machine: Apple M4 Pro, macOS 26.2,
  Apple clang 17, 10 P-cores and 4 E-cores, no SMT, no NUMA, no Linux
  `perf`. Each README states this and what an x86 server part would show
  differently. The NUMA and syscall benchmarks demonstrate the mechanism
  (first-touch fault cost, fixed kernel-crossing cost) rather than server
  figures.
- The final serial run took place with a load average of 7 to 11 from the
  user's own browser processes; every `results/raw.txt` records the load
  average at start, and the per-row cv column shows where it mattered.
  A re-run on a quiet machine is `./run_all.sh` in `misc/benchmarks/`.
- The `justine.lol/matmul/` entry uses `http://` because the site's TLS
  certificate expired on 2026-06-09; switch to `https://` once renewed.
- The Intel and Arm document libraries, ACM and a few others answer 403 to
  every automated client; the checker reports them as blocked and the
  weekly CI run will never turn them dead on its own. If one of those
  documents moves, a reader will notice before the checker does.
- CI on the first two pushes caught three things a Mac cannot: gcc needs
  `_GNU_SOURCE` for `CLOCK_MONOTONIC_RAW` under `-std=c11`, gcc counts a
  `+` asm operand twice against its limit of 30 (benchmark 03's sixteen
  chains are now two statements on x86), and `drkp.net` answered a TLS
  handshake too slowly from the runner once (the Tales of the Tail entry
  now points at the UW lab copy and the checker lengthens its timeout on
  each retry). All fixed before the last push; see the Actions tab.

## For the maintainer

- The repo is at the URL above with `main` as the only branch, private.
- `misc/notes/sections/*.md` are the drafts the README was assembled from;
  after this build the README is edited directly and the drafts are a
  record.
- To re-measure: `cd benchmarks && ./run_all.sh`, then commit
  `results/` and the README tables together and update the machine
  section of each README.
