# n09-ttlcache

*Area:* generics + borrows

A generic `TtlCache<K, V>` (`where K: hashable`) whose entries expire after a TTL measured by an injected `IClock` (a `FakeClock` the test advances). The cache holds the clock as a borrow given at construction (a helper back-reference). Sequence: put a=1 at t=0 (ttl 10), put b=2 at t=5, get a at t=8 -> 1, get a at t=12 -> miss (expired), get b at t=12 -> 2, count after purge -> 1. Print `get a@8 -> 1`, `get a@12 -> miss`, `get b@12 -> 2`, `live 1`. Compile also with `--report-trust-admissions` and record the report lines.
