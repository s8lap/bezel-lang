// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/getter_only_property_747_test.cpp
//   (BEZEL_TEST getter_only_property_747, set_only_compound_assign_rejected_1192).
//
// §23 (#1192): a compound `op=` on a set-only property READS first, so it is
// also rejected (the plain `:=` exemption does not extend to compound forms).

module M; class C public _x: int := 0; public property Val: int set => do _x := value; end; end; public static method Main(): int using var c := new C(); c.Val += 3; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: property `Val` is write-only (it declares only a `set` accessor) and cannot be read
// CHECK-SAME: (§23)
