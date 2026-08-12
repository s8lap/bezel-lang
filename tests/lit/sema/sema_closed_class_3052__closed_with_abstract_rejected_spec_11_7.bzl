// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §11.7 (v0.242, #3052), verbatim: "A `closed` class may declare abstract
// members (§12.5) without carrying the `abstract` modifier — `closed`
// subsumes it — and combining `closed` with `abstract`, `sealed`, or `static`
// is a compile error."

module M; closed abstract class Shape public abstract method Area(): int; end Shape;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `Shape` may not be both `closed` and `abstract`
// CHECK-SAME: (§11.7)
