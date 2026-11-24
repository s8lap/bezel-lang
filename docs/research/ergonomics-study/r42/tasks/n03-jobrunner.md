# n03-jobrunner

*Area:* errors + async

An async job runner (a boundary) runs four jobs in order with a shared 200 ms deadline: `ok` returns 1; `flaky` throws a user `TransientError` marked `IFailure`; `buggy` throws `InvalidOperationException`; `slow` awaits a 5 s delay with the token. The runner's arms are, in order: cancellation re-raised, `IFailure` logged as `flaky: transient: upstream 503`, any other `Exception` reported as `buggy: BUG InvalidOperationException: bad state`. Expected output: `ok: 1`, `flaky: transient: upstream 503`, `buggy: BUG InvalidOperationException: bad state`, then the cancellation propagates out of the runner and `Main` prints `runner stopped: cancelled`.
