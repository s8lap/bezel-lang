// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Appendix A.6 `AssignOp` (v0.242, #3052), verbatim: "`mod=` is one token,
// lexed by longest match: the keyword `mod` immediately followed by `=` with
// no intervening space. `x mod = y` remains a syntax error."
//
// The intervening space breaks the longest-match, so the tokens are the
// keyword `mod` and the equality operator `=` — a `mod` with no right operand.

module M; class C public static method Main(): int var x: int := 5; var y: int := 3; x mod = y; return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: expected expression
