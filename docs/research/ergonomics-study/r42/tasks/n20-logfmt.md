# n20-logfmt

*Area:* text

Parse logfmt lines (`key=value` pairs separated by spaces; values may be double-quoted with spaces inside and `\"` escapes): `level=info msg="user login" user=ann dur=12ms`, `level=error msg="db \"main\" down" dur=1500ms`, `level=info msg=ok dur=3ms`. Print per level the count (`info 2`, `error 1`), the slowest message with its duration in ms (`slowest: db "main" down (1500 ms)`), and `keys: dur, level, msg, user` (sorted distinct keys).
