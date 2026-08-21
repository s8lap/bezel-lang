// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11, spec v0.220): the §6.6 compound family check was gated
// on both operands being NON-nullable, so `int? += double?` was never checked.
// Per §7.11 the lifted `int? + double?` computes `double?` (§6.2 promotes int
// to double), which is neither `int?` nor implicitly convertible to it, and
// the §6.6 write-back covers integer results only — "in every other case the
// compound form is a compile error". Before the fix this reached the #1964
// nullable lowering, which fed the double payload to the integer overflow
// intrinsic — a malformed-IR compiler abort instead of a diagnostic.

module M; class C public static method Main(): int var i: int? := 5; var d: double? := 0.5; i += d; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not defined when both operands are nullable (target `int?`, right-hand operand `double?`) — the arithmetic, bitwise, or shift operators do not lift over a nullable value type (§7.11, v0.267); narrow first (§7.4 check, `?? <fallback>` (§7.7), or `!` (§7.8)) and use the non-nullable compound form (e.g. `b := (b ?? 0) + 1;`) (§6.6)
