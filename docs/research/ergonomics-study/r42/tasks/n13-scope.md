# n13-scope

*Area:* async

Fetch three simulated endpoints under a `TaskScope` with a 300 ms deadline: `a` (20 ms -> "A"), `b` (40 ms -> "B"), `c` (60 ms -> fails with a user `UpstreamError` marked `IFailure`). Results are written into an array slot per child (value index captured). The first fault cancels siblings and re-raises at `Join`; the caller catches the `IFailure` and prints `failed: upstream c` and the results that completed before it (`done: A B`). A second run with `c` succeeding prints `done: A B C`.
