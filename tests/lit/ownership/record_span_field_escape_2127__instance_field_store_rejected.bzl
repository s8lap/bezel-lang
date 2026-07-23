// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 (issue #2127 fallout): once a span-field record is borrow-bearing, the
// §9.7 instance-field-store rule applies to it — "A borrow-bearing record value
// may not be stored in an instance field of a class." A class instance's
// lifetime is not bounded by any borrow source's scope, and a span's buffer
// window is consumed through access paths with no per-access disposed-flag
// check, so the store is rejected statically. The null-source exemption cannot
// apply (a span field is never null), which the fix enforces by also teaching
// isNullSourceRecordExpr to see the span field (else a vacuous "no borrow field"
// verdict would bypass this rejection). // per §9.7

module M; record Holder(S: ArraySpan<int>); class Box public r: Holder; public constructor(x: Holder) r := x; end constructor; end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing record may not be stored in an instance field of a class
// CHECK-SAME: (§9.7
