# n22-todo

*Area:* CLI

A todo CLI over a file in a scratch directory: `todo add <text...>` appends a task, `todo list` prints `1. [ ] buy milk` lines, `todo done <n>` marks it done (`[x]`), `todo rm <n>` removes it; a bad index prints `todo: no item 9` to stderr and exits 1; no args prints usage and exits 2. Driver run: add "buy milk", add "write report", done 1, list -> `1. [x] buy milk`, `2. [ ] write report`; done 9 -> exit 1; rm 1, list -> `1. [ ] write report`.
