// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, duplicate_field_in_rejected_class_with_both_diagnose_935).
//
// REJECT (v0.188, #1735): a duplicate field name on a now-rejected
// class-typed `with` source still gets its OWN "assigned more than once"
// diagnostic — the dup-name check (this section) runs independently of the
// §9.5 source-type validation above (see ExprTypecheck_Exprs.cpp): a
// program with both defects sees both diagnostics, not just the source-type
// one. This pins that accumulate-diagnostics behavior (was previously
// exercised on the accepted §22.11 class object-initializer form, now
// retired — the source-type rejection fires ADDITIONALLY here).

module M; class Box public property V: int; public constructor() V := 0; end constructor; destructor end destructor; end Box; class C public static method Main(): int using var b := new Box() with V := 3, V := 4 end; return b.V; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `with` requires a record-typed source; this source is class-typed
// CHECK-SAME: (§9.5, v0.188)
