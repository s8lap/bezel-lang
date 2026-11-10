# t05-ini

*Area:* CLI/text

Parse an embedded INI document with `[section]` headers, `key = value` lines (whitespace trimmed), blank lines and comments starting with `;` or `#`. Store as section -> (key -> value). Print each section name followed by its keys in file order as `  key=value`. Then look up `server.port` as an integer (default 80 if missing or not a number) and `server.timeout` (missing; default 30) and print `port=8080 timeout=30`. A line that is neither prints `line N: syntax error` and is skipped.
