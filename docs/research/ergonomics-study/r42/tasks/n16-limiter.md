# n16-limiter

*Area:* async

Limit concurrency with a `Semaphore(2)`: start six async jobs, each acquires a permit with `AcquireAsync`, records the current/maximum number of jobs inside the critical section, delays 20 ms, and releases. Print `max concurrent 2` and `completed 6`.
