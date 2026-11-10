# t33-fanout

*Area:* async & concurrency

Fetch three simulated sources concurrently (async methods that delay 30/10/20 ms and return 1, 2, 3). Await all, print `results 1 2 3` in source order and `sum 6`; the elapsed wall time should be closer to the max than the sum (print `concurrent: True` if elapsed < 55 ms).
