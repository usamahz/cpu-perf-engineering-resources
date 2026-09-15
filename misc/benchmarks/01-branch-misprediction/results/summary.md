Clock estimate 4.49 GHz (dependent 1-cycle add chain, 400000000 adds, min of 7 runs). Array of 4194304 32-bit elements, 16777216 bytes. Median over the timed passes; cycles per element is median ns per element times the clock estimate.

| variant | condition true | median ns/elem | min ns/elem | cycles/elem | cv |
|---|---|---|---|---|---|
| t128 unsorted branchy | 50.0 % | 2.386 | 2.363 | 10.71 | 0.7 % |
| t128 unsorted branchless | 50.0 % | 0.226 | 0.226 | 1.02 | 4.1 % |
| t128 unsorted vector | 50.0 % | 0.056 | 0.056 | 0.25 | 5.0 % |
| t128 sorted branchy | 50.0 % | 0.225 | 0.224 | 1.01 | 1.1 % |
| t128 sorted branchless | 50.0 % | 0.226 | 0.226 | 1.02 | 0.8 % |
| t128 sorted vector | 50.0 % | 0.056 | 0.056 | 0.25 | 1.2 % |
| t243 unsorted branchy | 5.1 % | 0.508 | 0.507 | 2.28 | 1.1 % |
| t243 unsorted branchless | 5.1 % | 0.227 | 0.226 | 1.02 | 1.0 % |
| t243 unsorted vector | 5.1 % | 0.056 | 0.056 | 0.25 | 2.7 % |
| t243 sorted branchy | 5.1 % | 0.223 | 0.223 | 1.00 | 0.9 % |
| t243 sorted branchless | 5.1 % | 0.227 | 0.226 | 1.02 | 1.0 % |
| t243 sorted vector | 5.1 % | 0.056 | 0.056 | 0.25 | 3.3 % |

| derived | value | unit |
|---|---|---|
| t128 branchy unsorted / branchy sorted | 10.62 | ratio |
| t128 branchy unsorted / branchless unsorted | 10.54 | ratio |
| t128 branchy sorted / branchless sorted | 0.99 | ratio |
| t128 branchless unsorted / vector unsorted | 4.06 | ratio |
| t128 extra cycles per element, branchy unsorted minus branchy sorted | 9.70 | cycles |
| t128 implied cost per mispredict (extra cycles / 0.4995 mispredict rate) | 19.4 | cycles |
| t243 branchy unsorted / branchy sorted | 2.27 | ratio |
| t243 branchy unsorted / branchless unsorted | 2.24 | ratio |
| t243 branchy sorted / branchless sorted | 0.99 | ratio |
| t243 branchless unsorted / vector unsorted | 4.07 | ratio |
| t243 extra cycles per element, branchy unsorted minus branchy sorted | 1.28 | cycles |
| t243 implied cost per mispredict (extra cycles / 0.0505 mispredict rate) | 25.3 | cycles |
