// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, bool_not_parsable_constraint).
//
// §2.19: `bool` conforms to none of the three.

module M; static class F public static method P<T>(s: string): T where T: IParsable<T> return T.Parse(s); end P; end F; class C public static method Main(): int var b := F.P<bool>("true"); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `bool` does not satisfy the constraint `where T: IParsable`
// CHECK-SAME: (§14.2)
