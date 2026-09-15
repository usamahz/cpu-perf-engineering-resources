4194304 records: the array of structs is 268435456 bytes (64 per record) and each of the sixteen field arrays is 16777216 bytes (4 per record); every timed pass starts after a 134217728-byte sweep that evicts the caches, so the data comes from memory. Median over the timed passes; effective GB/s is the bytes the kernel has to bring in per record (64 for the struct layout, 4 or 8 for the field arrays) divided by the median ns per record. Cycles per record: every pass is converted with the clock sampled just before it (min of three short dependent integer add chains), then the median of the conversions is taken; the per-variant median clock ranged from 4.512 to 4.513 GHz, and common/clock_estimate read 4.50 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs) just before the run. Max rel err is the largest deviation of any pass from the double-precision reference (sum3 2096667.738, dot37 1048326.786; a deviation above 1e-02 fails the run).

| kernel | variant | bytes/rec | median ns/rec | min ns/rec | cycles/rec | effective GB/s | cv | max rel err |
|---|---|---|---|---|---|---|---|---|
| sum3 | aos_scalar | 64 | 0.6685 | 0.6501 | 3.017 | 95.7 | 6.6 % | 5.53e-05 |
| sum3 | aos_o2 | 64 | 0.6247 | 0.6025 | 2.819 | 102.4 | 1.7 % | 5.91e-07 |
| sum3 | soa_scalar | 4 | 0.5552 | 0.5543 | 2.505 | 7.2 | 1.3 % | 5.53e-05 |
| sum3 | soa_strict | 4 | 0.4737 | 0.4735 | 2.138 | 8.4 | 1.9 % | 5.53e-05 |
| sum3 | soa_o2 | 4 | 0.0439 | 0.0422 | 0.198 | 91.1 | 2.1 % | 5.91e-07 |
| sum3 | soa_neon | 4 | 0.0456 | 0.0445 | 0.206 | 87.7 | 4.4 % | 5.91e-07 |
| dot37 | aos_scalar | 64 | 0.7871 | 0.7776 | 3.547 | 81.3 | 8.3 % | 1.90e-03 |
| dot37 | aos_o2 | 64 | 0.6583 | 0.6388 | 2.969 | 97.2 | 3.1 % | 1.49e-04 |
| dot37 | soa_scalar | 8 | 0.7647 | 0.7452 | 3.449 | 10.5 | 3.8 % | 1.90e-03 |
| dot37 | soa_strict | 8 | 0.4827 | 0.4825 | 2.178 | 16.6 | 3.4 % | 1.90e-03 |
| dot37 | soa_o2 | 8 | 0.0619 | 0.0613 | 0.279 | 129.3 | 2.5 % | 9.57e-06 |
| dot37 | soa_neon | 8 | 0.0621 | 0.0617 | 0.280 | 128.9 | 2.7 % | 9.57e-06 |

| derived | value | unit |
|---|---|---|
| sum3 bytes per record, aos / soa | 16.00 | ratio |
| sum3 aos_scalar / soa_scalar, median ns/rec | 1.20 | ratio |
| sum3 aos_o2 / soa_o2, median ns/rec | 14.23 | ratio |
| sum3 aos_o2 / soa_neon, median ns/rec | 13.70 | ratio |
| sum3 aos_scalar / aos_o2, median ns/rec | 1.07 | ratio |
| sum3 soa_scalar / soa_o2, median ns/rec | 12.65 | ratio |
| sum3 soa_scalar / soa_strict, median ns/rec | 1.17 | ratio |
| sum3 soa_strict / soa_o2, median ns/rec | 10.79 | ratio |
| sum3 soa_o2 / soa_neon, median ns/rec | 0.96 | ratio |
| dot37 bytes per record, aos / soa | 8.00 | ratio |
| dot37 aos_scalar / soa_scalar, median ns/rec | 1.03 | ratio |
| dot37 aos_o2 / soa_o2, median ns/rec | 10.63 | ratio |
| dot37 aos_o2 / soa_neon, median ns/rec | 10.60 | ratio |
| dot37 aos_scalar / aos_o2, median ns/rec | 1.20 | ratio |
| dot37 soa_scalar / soa_o2, median ns/rec | 12.35 | ratio |
| dot37 soa_scalar / soa_strict, median ns/rec | 1.58 | ratio |
| dot37 soa_strict / soa_o2, median ns/rec | 7.80 | ratio |
| dot37 soa_o2 / soa_neon, median ns/rec | 1.00 | ratio |
| clock, per-variant median of the per-pass samples, lowest / highest | 4.512 / 4.513 | GHz |
| clock, common/clock_estimate before the run | 4.50 | GHz |
