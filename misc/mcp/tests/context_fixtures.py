"""Tool output as users paste it, in the shapes perf, toplev, gcc and clang print."""

PERF5 = """
 Performance counter stats for './bench':

            812.34 msec task-clock                #    0.998 CPUs utilized
                 3      context-switches          #    0.004 K/sec
                 0      cpu-migrations            #    0.000 K/sec
               104      page-faults               #    0.128 K/sec
     2,537,513,000      cycles                    #    3.124 GHz
     1,015,005,200      instructions              #    0.40  insn per cycle
       381,153,000      branches                  #  469.209 M/sec
        12,856,000      branch-misses             #    3.37% of all branches
       400,000,000      L1-dcache-loads
        40,000,000      L1-dcache-load-misses     #   10.00% of all L1-dcache accesses
     1,500,000,000      stalled-cycles-backend    #   59.11% backend cycles idle

       0.813955300 seconds time elapsed

       0.810000000 seconds user
       0.004000000 seconds sys
"""

PERF6_HYBRID = """
 Performance counter stats for './server --bench':

          1,234.56 msec task-clock:u                     #    0.998 CPUs utilized
                 0      context-switches:u               #    0.000 /sec
               123      page-faults:u                    #   99.633 /sec
     4,567,890,123      cpu_core/cycles/u                #    3.700 GHz                         (83.33%)
     <not counted>      cpu_atom/cycles/u                                                       (0.00%)
     9,876,543,210      cpu_core/instructions/u          #    2.16  insn per cycle              (83.33%)
     <not counted>      cpu_atom/instructions/u                                                 (0.00%)
     1,111,111,111      cpu_core/branches/u              #  900.000 M/sec                       (83.33%)
        12,345,678      cpu_core/branch-misses/u         #    1.11% of all branches             (83.33%)
             TopdownL1 (cpu_core)                 #     29.3 %  tma_backend_bound
                                                  #     10.1 %  tma_bad_speculation
                                                  #     20.6 %  tma_frontend_bound
                                                  #     40.0 %  tma_retiring             (83.33%)
             TopdownL1 (cpu_atom)                 #     35.0 %  tma_backend_bound
                                                  #     25.0 %  tma_frontend_bound

       1.237123456 seconds time elapsed
"""

PERF_DE_LOCALE = """
 Performance counter stats for './a.out':

     2.000.000.000      cycles
     3.000.000.000      instructions              #    1,50  insn per cycle

       1,000000000 seconds time elapsed
"""

PERF_CSV = """1234.56,msec,task-clock,1234560000,100.00,0.998,CPUs utilized
4567890123,,cycles,1234000000,100.00,3.700,GHz
9876543210,,instructions,1234000000,100.00,2.16,insn per cycle
<not supported>,,stalled-cycles-frontend,0,100.00,,
123456,,cpu/event=0x3c,umask=0x0/,1234000000,50.00,,
"""

PERF_CSV_INTERVAL = """1.000123456,1000000000,,cycles,1000000000,100.00,,
1.000123456,500000000,,instructions,1000000000,100.00,,
2.000234567,1000000000,,cycles,1000000000,100.00,,
2.000234567,700000000,,instructions,1000000000,100.00,,
"""

PERF_JSON = """{"counter-value" : "4567890123.000000", "unit" : "", "event" : "cycles", "event-runtime" : 1234000000, "pcnt-running" : 100.00, "metric-value" : "3.700000", "metric-unit" : "GHz"}
{"counter-value" : "2283945061.000000", "unit" : "", "event" : "instructions", "event-runtime" : 1234000000, "pcnt-running" : 61.50, "metric-value" : "0.500000", "metric-unit" : "insn per cycle"}
{"counter-value" : "<not counted>", "unit" : "", "event" : "LLC-load-misses", "event-runtime" : 0, "pcnt-running" : 0.00}
"""

PERF_OLD_TOPDOWN = """
 Performance counter stats for 'system wide':

                                retiring      bad speculation       frontend bound        backend bound
S0-D0-C0           2                 30.0%                 4.0%                26.0%                40.0%
S0-D0-C1           2                 32.0%                 6.0%                22.0%                40.0%

       1.001234567 seconds time elapsed
"""

PERF_AMD_PIPELINE = """
 Performance counter stats for './bench':

             PipelineL1                      #     18.5 %  frontend_bound
                                             #     45.0 %  backend_bound
                                             #      2.5 %  bad_speculation
                                             #     34.0 %  retiring

       1.000000000 seconds time elapsed
"""

TOPLEV = """# 4.8-full-perf on Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz [skl]
FE             Frontend_Bound                  % Slots                       12.3    [ 9.0%]
BAD            Bad_Speculation                 % Slots                        2.1  < [ 9.0%]
BE             Backend_Bound                   % Slots                       60.6    [ 9.0%]
RET            Retiring                        % Slots                       25.0  < [ 9.0%]
BE/Mem         Backend_Bound.Memory_Bound      % Slots                       45.2    [ 9.0%] <==
"""

GCC_REMARKS = """kernel.c:12:5: missed: couldn't vectorize loop
kernel.c:14:9: missed: not vectorized: complicated access pattern.
kernel.c:21:5: missed: not vectorized: possible aliasing between a and b
kernel.c:30:5: optimized: loop vectorized using 32 byte vectors
"""

CLANG_REMARKS = """kernel.c:12:5: remark: loop not vectorized [-Rpass-missed=loop-vectorize]
kernel.c:12:5: remark: loop not vectorized: cannot identify array bounds [-Rpass-analysis=loop-vectorize]
kernel.c:30:5: remark: vectorized loop (vectorization width: 8, interleaved count: 4) [-Rpass=loop-vectorize]
"""

OBJDUMP = """0000000000001139 <sum>:
    1139:	c5 fc 57 c0          	vxorps %ymm0,%ymm0,%ymm0
    113d:	c4 e2 7d 92 04 87    	vgatherdps %ymm1,(%rdi,%ymm2,4),%ymm0
    1143:	c5 fc 58 04 87       	vaddps (%rdi,%rax,4),%ymm0,%ymm0
    1148:	f0 48 0f c1 07       	lock xadd %rax,(%rdi)
    114d:	c5 f8 77             	vzeroupper
"""

PERF_ANNOTATE = """ Percent |      Source code & Disassembly of bench for cycles:u
         :      for (i = 0; i < n; i++)
   12.50 │      vmovups   (%rax,%rdx,4),%zmm0
   40.00 │      vfmadd231ps (%rcx,%rdx,4),%zmm1,%zmm0
   20.00 │      vmovups   %zmm0,(%rax,%rdx,4)
    5.00 │      add       $0x10,%rdx
"""

CODE = """#include <atomic>
#include <immintrin.h>
struct alignas(64) Counter { std::atomic<long> n; };
void bump(Counter *c) { c->n.fetch_add(1, std::memory_order_relaxed); }
void axpy(float *__restrict y, const float *x, float a, int n) {
    __m256 va = _mm256_set1_ps(a);
    for (int i = 0; i < n; i += 8) {
        _mm256_storeu_ps(y + i, _mm256_fmadd_ps(va, _mm256_loadu_ps(x + i), _mm256_loadu_ps(y + i)));
    }
}
"""
