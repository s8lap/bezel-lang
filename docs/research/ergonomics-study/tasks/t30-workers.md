# t30-workers

*Area:* async & concurrency

A worker pool: 4 workers run in parallel domains; each receives a slice of the numbers 1..1000 and returns the sum of squares of its slice; the main program combines the partial results and prints `sum of squares 333833500` and `workers 4`.
