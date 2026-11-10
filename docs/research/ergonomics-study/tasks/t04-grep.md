# t04-grep

*Area:* CLI/text

A grep-like filter: arguments are `[-i] [-v] PATTERN`; read lines from stdin and print `N:line` for each line containing PATTERN (substring), case-insensitively with `-i`, inverted with `-v`; exit code 0 if anything matched, 1 otherwise; usage error (no pattern) prints `usage: grep [-i] [-v] PATTERN` to stderr and exits 2. Input: `alpha\nBeta\ngamma\nbetamax\n`; with args `-i beta` expected `2:Beta` and `4:betamax`, exit 0; with `-v a` nothing is printed (every line contains `a`) and the exit code is 1.
