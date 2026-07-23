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

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: `int? + double?` computes `double?`, which is neither `int?` nor implicitly convertible to it, and `int?` is not an integer/char target receiving an integer result (the only case §6.6's write-back narrows) — convert the right-hand operand explicitly, or write `x := int?(x + e);` if the loss is intended
// CHECK-SAME: (§6.6)
