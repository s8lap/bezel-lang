# n01-config

*Area:* errors (v0.359)

Load two embedded configuration texts of `key = value` lines. `port` is required: a missing or non-numeric port throws a user exception `ConfigError` (marked `IFailure`, since the operator wrote the file) from the throwing form `RequirePort`; `workers` is optional and defaults to 4 through `ParseOrNull`. `Main` is the one boundary: it loads config A (`port = 8080`, `workers = x`) and prints `A: port=8080 workers=4`, then loads config B (`workers = 2`, no port) and prints `B: configuration: missing port` to stdout and returns exit code 2.
