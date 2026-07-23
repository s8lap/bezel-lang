// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §6.5, spec v0.220): `int &= long?` — the bitwise
// operators lift over nullable INTEGER operands (§7.11), so `int & long?` is
// `long?`, which cannot store back into the non-nullable `int` target. The
// same nullable-RHS store-back rule as the arithmetic forms, on the
// bitwise/shift branch (which previously checked only the operand KINDS and
// only for non-nullable operands).

module M; class C public static method Main(): int var i: int := 7; var w: long? := 3L; i &= w; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `&=` has a nullable right-hand operand (`long?`) with a non-nullable target (`int`)
// CHECK-SAME: (§6.6)
