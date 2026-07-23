// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §3.5, spec v0.220): `int? += bool?` — no operator `+`
// is defined between int and bool (§3.5: arithmetic is defined on numeric
// operands; §5.1: bool never converts to an integer), so the lifted compound
// has no valid result. Before the fix the nullable gate skipped the check and
// the #1964 lowering SExt'd the i1 payload — `int?(5) += bool?(true)` silently
// stored 4 (true → -1), a garbage value.

module M; class C public static method Main(): int var i: int? := 5; var b: bool? := true; i += b; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: `int? + bool?` computes `bool?`, which is neither `int?` nor implicitly convertible to it
// CHECK-SAME: (§6.6)
