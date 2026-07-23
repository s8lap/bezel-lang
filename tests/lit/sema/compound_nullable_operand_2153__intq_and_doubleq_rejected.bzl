// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.5 / §7.11, spec v0.220): `int? &= double?` — the bitwise
// operators are not valid on floating-point operands (§6.5), and §7.11 lifts
// an operator only where it is defined on the underlying types. The §6.5
// right-hand-operand kind check existed (#1689) but was gated on both
// operands being non-nullable, so the nullable form went undiagnosed.

module M; class C public static method Main(): int var i: int? := 7; var d: double? := 1.5; i &= d; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `&=` is not valid on floating-point types — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)
