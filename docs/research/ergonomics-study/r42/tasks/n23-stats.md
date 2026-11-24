# n23-stats

*Area:* CLI

A `stats` CLI reading one number per line from stdin, skipping blank lines and reporting unparseable lines to stderr as `stats: line 4: not a number: abc` (using `double.ParseOrNull`); flag `-p N` sets the decimal places (default 2). Input `4
8

abc
15
16
23
42
` with `-p 1`: prints `n=6 min=4.0 max=42.0 mean=18.0 median=15.5`.
