# Section owner brief

You own one section of a CPU performance engineering resource list. The
README is the product. Your output is one markdown file in
`misc/notes/sections/`. A single editor merges the files; you do not touch the
README.

## Non-negotiable rules

1. **Primary sources only.** Original papers, vendor optimisation manuals
   and ISA references, creator repositories, kernel and compiler
   documentation, direct implementer reports with code and measurements.
   No blog aggregators, listicles, SEO summaries, AI-generated explainers,
   survey papers, slide decks without a paper, or Wikipedia. A personal
   blog post is admissible only when it is the original report of a
   mechanism or a measurement by the person who did the work.
2. **One line per entry**, exactly this form, nothing else on the line:

       - [Title](URL) - Why it earns its place.

   Hyphen, space, link, space, hyphen, space, reason, full stop. The reason
   says why this source and not another: what it uniquely establishes,
   measures, defines or proves. It does not summarise the source and does
   not name the author. Keep it under 150 characters after the hyphen.
3. **Every performance number needs all seven fields** in the source it
   comes from: CPU model and microarchitecture; core count used;
   frequency with turbo and SMT state; compiler and flags; workload;
   baseline; method. Do not put numbers in annotations. If an entry's
   value rests on a number, list it under `## Claims` with the seven
   fields filled from the source, or mark which are missing.
4. **Every URL is live and canonical.** Verify each with
   `curl -sIL -o /dev/null -w '%{http_code} %{url_effective}\n' URL`
   (add `-A 'Mozilla/5.0'` if you get 403). Use the final URL when a
   redirect is permanent. Official documentation over mirrors; arXiv `abs`
   page over `pdf`; author's own page over a paywalled copy; the GitHub
   repository for software; the vendor's own document library for
   manuals. If a site returns 403 or 429 to curl, confirm with WebFetch
   that the page is the resource, and note it under `## Link notes`.
5. **No content from the GPU reference list**, no GPU entries, no CUDA.
6. **No em dashes** anywhere. British spelling in prose (optimise,
   behaviour, neighbours), except `quantization`, and except inside
   titles, which keep their own spelling.
7. **No voice imitation.** Read the corpus authors for rigour and
   coverage, write the reasons in plain, direct language of your own.

## Coverage duty

Your section names a corpus: authors and documents that must appear. Read
their primary work, apply their standard of evidence, and make sure each
appears with its canonical primary source. Add what else the section
needs to be complete for a newcomer who reads the list top to bottom.

## Size

Two to four H3 subsections per H2. Three to six entries per H3, hard cap
seven. Order inside an H3 is dependency order (read this before that), not
importance and not date. If a section needs a one-sentence preamble
(a warning the reader needs before the links), write it under the H2.

## File format

`misc/notes/sections/NN-slug.md`:

```
<!-- owner: NAME -->
## N. Section title

Optional one-sentence preamble.

### Subsection

- [Title](URL) - Reason.

### Subsection

- [Title](URL) - Reason.

## Rejected

- [Title](URL) - Rule it fails and why.

## Claims

- "<number as stated>" from <URL>: fields present: 1 CPU ..., 2 cores ...,
  3 freq/turbo/SMT ..., 4 compiler/flags ..., 5 workload ..., 6 baseline
  ..., 7 method ... Missing: <list or none>. Verdict: core / watchlist / cut.

## Link notes

- <URL>: curl 403, confirmed by fetch; or: redirected from <old> to <new>.

## Benchmark proposal

One claim from this section that can be reproduced on an Apple M4 Pro
(macOS 26, Apple clang 17, arm64, NEON, SME2, no SMT, 10 P-cores plus
4 E-cores, no NUMA, no Linux perf). Give: the claim in one sentence, the
program in one paragraph, what the numbers should show, and which entry
in the section it supports.
```

Keep `## Rejected` honest and specific: it becomes the public changelog of
what was considered and why it was left out.
