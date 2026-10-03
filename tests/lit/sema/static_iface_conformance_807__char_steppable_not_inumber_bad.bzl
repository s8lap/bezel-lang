// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, char_steppable_not_inumber —
//   second of two independent parseSemaAll() assertions in that BEZEL_TEST
//   body, split into its own .bzl per the extractor's manual-split
//   convention).
//
// §2.19: `char` conforms to ISteppable but NOT to INumber. This half checks
// the INumber-constrained call is rejected.

module M; static class F   public static method Add2<T>(a: T, b: T): T where T: INumber<T>     return a + b; end Add2; end F; class C public static method Main(): int   var c := F.Add2('a', 'b'); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `char` does not satisfy the constraint `where T: INumber`
// CHECK-SAME: (§14.2)
