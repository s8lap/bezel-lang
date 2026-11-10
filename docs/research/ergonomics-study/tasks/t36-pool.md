# t36-pool

*Area:* resources

A connection pool that owns up to 2 Connection objects (each prints `open N` when created and `close N` when disposed). Lease a connection, use it (`conn 1: query`), return it to the pool; lease twice concurrently-in-scope (forcing a second connection); at program end the pool disposes every connection exactly once (`close 1`, `close 2`, either order). Leasing must be exception-safe: an exception while using a leased connection still returns it.
