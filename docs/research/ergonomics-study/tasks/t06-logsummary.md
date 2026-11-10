# t06-logsummary

*Area:* CLI/text

Parse five embedded log lines of the form `2026-01-05T10:02:30 ERROR disk full` (ISO timestamp, level, message). Count lines per level and print `LEVEL count` in order of first appearance (`INFO 2`, `ERROR 2`, `WARN 1`). Then print the first and last ERROR timestamps and the elapsed time between them as `first=10:02:30 last=10:15:45 span=00:13:15`.
