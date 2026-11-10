# t20-exceptions

*Area:* errors

Define a `ConfigError` exception that wraps a lower-level cause. Loading config parses `port` from text `"80a"`; the `FormatException` from int.Parse is wrapped as `ConfigError("invalid port")`. The caller catches ConfigError, prints `error: invalid port`, then prints the cause chain entries as `  caused by: FormatException`. Also demonstrate `finally` by printing `cleanup` once.
