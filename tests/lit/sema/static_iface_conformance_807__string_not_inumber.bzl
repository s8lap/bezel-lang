// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, string_not_inumber).
//
// §2.19 conformance table at the call site: `string` conforms to none.

module M; static class F public static method Add2<T>(a: T, b: T): T where T: INumber<T> return a + b; end Add2; end F; class C public static method Main(): int var s := F.Add2("a", "b"); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `string` does not satisfy the constraint `where T: INumber`
// CHECK-SAME: (§14.2)
