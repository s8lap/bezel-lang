# n05-exceptions

*Area:* errors (v0.359)

A small storage layer with its own exception hierarchy: `StorageError(message): Exception, IFailure`, and `NotFoundError(key)` deriving from it, plus a `CorruptRecordError(key, cause)` that wraps a `FormatException` with a cause (constructor with `(message, cause)`). `Get(key)` throws `NotFoundError` for an unknown key and `CorruptRecordError` when the stored text is not a number. A `GetOrNull` non-throwing dual is offered for callers expecting absence. Expected: `a = 1`, `b -> (absent)` via GetOrNull, `get b: not found: b` (caught as `NotFoundError`), `get c: corrupt record c` and `  caused by System.FormatException` (caught as the `IFailure` base, then cause chain printed).
