// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/compound_assign_cross_family_1689_test.cpp
//   (BEZEL_TEST compound_assign_cross_family_1689, byte_plus_double_rejected).
//
// §6.6: types-probes/q1_byte_plus_double.bzl — `b += 1.5` on `b: byte`.
// R = double (the promoted type of `byte + double`); double does not
// implicitly convert to byte, and byte/double is not the integer-target/
// integer-result write-back pair, so this is a compile error. Before the
// fix this passed the frontend and crashed LLVM IR verification
// (`zext double ... to i32` — ZExt requires an integer source).

module M; class C public static method Main(): int var b: byte := 250; b += 1.5; return int(b); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: `byte + double` computes `double`, which is neither `byte` nor implicitly convertible to it, and `byte` is not an integer/char target receiving an integer result (the only case §6.6's write-back narrows) — convert the right-hand operand explicitly, or write `x := byte(x + e);` if the loss is intended
// CHECK-SAME: (§6.6)
