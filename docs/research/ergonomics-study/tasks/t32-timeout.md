# t32-timeout

*Area:* async & concurrency

Run a slow async operation (an async loop of 50 x 20 ms delays checking a cancellation token) under a 100 ms timeout. Print `timed out` when cancelled. Run a fast variant (2 x 10 ms) under the same timeout and print `fast result 2`.
