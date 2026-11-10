# t18-retry

*Area:* errors

A flaky operation that throws `IOException("transient")` on its first two calls and returns 42 on the third. A generic retry helper calls it up to 3 times with a small delay (10 ms, then 20 ms) between attempts and prints `attempt 1 failed: transient`, `attempt 2 failed: transient`, `result 42`. With max 2 attempts the helper rethrows and the caller prints `gave up: transient`. Written async (`Task.Delay`).
