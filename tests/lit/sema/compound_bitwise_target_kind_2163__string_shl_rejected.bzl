// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2163 (§6.5 / §3.5, spec v0.220): a `string` TARGET of a compound shift.
// §3.5 defines the shift operators "on the integer types" only, so `s <<= 2`
// has no §6.6 result. The §6.5 target-kind check listed bool/char/float/double
// but not string: before the fix this crashed the compiler outright
// (cast<IntegerType> assertion, core dump — probe, main @dd33abd).

module M; class C public static method Main(): int var s: string := "a"; s <<= 2; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `<<=` is not valid on `string` — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)
