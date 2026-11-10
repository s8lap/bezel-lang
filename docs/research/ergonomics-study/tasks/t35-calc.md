# t35-calc

*Area:* parsing

A tokenizer, recursive-descent parser to an AST, and evaluator for integer arithmetic with + - * / unary minus and parentheses. Evaluate `1 + 2 * 3` -> `7`, `(1 + 2) * 3` -> `9`, `-4 + 10 / 3` -> `-1`, `2 * (3 + ` -> `error: unexpected end at 10`, `8 / (4 - 4)` -> `error: division by zero`. Print the AST of the first expression as `(+ 1 (* 2 3))`.

*Erratum (found while running):* the input `2 * (3 + ` has length 9, so the expected position is 9, not 10.
