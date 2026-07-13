// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/enum_int_conversion_924_test.cpp
//   (BEZEL_TEST enum_int_conversion_924, new_on_nested_enum_rejected_960).
//
// §5.4 / §10 (#960): nested `new Outer.E(...)` is rejected too.

module M; public class Outer enum E A, B, C; end E; public static method Main(): int var e := new Outer.E(1); return 0; end Main; end Outer;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: nested type declarations require explicit visibility
// CHECK-SAME: (§13.3, §11.6)
