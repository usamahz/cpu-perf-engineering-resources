# One voice

The README has one author. Every reason line reads as if the same person
wrote it in one sitting. This is the style the editor enforces.

## The reason line

- One sentence or fragment, capital letter to full stop, under 150
  characters after the hyphen.
- It says why this source and not another: what it uniquely establishes,
  defines, measures, proves, or is the home of. Not what it is about.
  "The Intel manual on caches" describes. "Where Intel states the
  prefetcher rules a loop has to obey to be prefetched" justifies.
- No author names, no dates, no numbers, no adjectives of praise
  (comprehensive, essential, excellent, seminal, must-read, great,
  powerful, deep), no "this", no "here", no exclamation marks, no
  rhetorical questions.
- Plain verbs: defines, measures, shows, proves, states, is where,
  is the origin of, is the only, sets, fixes, explains why.
- The reader is addressed nowhere. No "you", no "we".
- British spelling in prose: optimise, behaviour, neighbour, centre,
  analyse, favour, licence (noun). Exception: quantization, and words
  inside titles, which keep the source's spelling.
- No em dashes, no en dashes as separators, no semicolons in reasons.

## Terms

out-of-order, in-order, micro-op (uop only inside a title), cache line,
x86 (never X86 or x86-64 unless the ISA width matters, then x86-64), Arm
(the company and the architecture; ARM only inside titles), AArch64 only
when naming the ISA state, NEON, SVE, SVE2, SME, SME2, AVX2, AVX-512,
AVX10, AMX, VNNI, RVV, io_uring, Linux, macOS, GEMM, BLAS, p99, p99.9,
NUMA, TLB, SMT (not hyperthreading), PMU, DVFS (not turbo unless quoting
Intel), first touch, false sharing, tail latency, roofline, top-down.

## Titles

The source's own title, trimmed to the part a reader would search for.
Manuals: "Intel 64 and IA-32 Architectures Optimization Reference Manual"
may be "Intel Optimization Reference Manual". Papers: the paper title.
Repositories: the project name. Talks: the talk title. No quotes around
titles, no trailing punctuation inside the brackets.

## Section preambles

At most two sentences, only where a reader needs a warning before the
links. Same voice as the reasons.

## Watchlist lines

"- [Title](URL) - What it is, and the condition that promotes it."
The condition uses until, pending, once or when.
