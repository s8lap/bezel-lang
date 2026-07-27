// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the field-INITIALIZER counterpart
// (checkFieldInitializer) — a directly-choice-typed instance field whose
// choice is borrow-bearing must also reject the store (parallel to the
// record-leg test of #1540). The instance-field rule applies to the
// initializer form as well as to a constructor-body assignment.
//
// The initializer `mkBorrow()` is a method call returning a non-null
// `SpanC` (carries a `ArraySpan<int>` borrow); per §9.7's null-source
// exception, the payload-free `SpanC.None` form WOULD be admitted, so
// this test uses a method-call source to pin the REJECT path.

module M; choice SpanC Some(s: ArraySpan<int>), None; end SpanC; class Box public static method mkBorrow(): SpanC return SpanC.Some(new Array<int>().AsSpan()); end mkBorrow; public r: SpanC := Box.mkBorrow(); end Box; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing choice may not be stored in the instance field `r`
// CHECK-SAME: (§9.7
// CHECK-SAME: §35.4
