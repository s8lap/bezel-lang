// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §12.5 (v0.242, #3052), verbatim: "`new` on an abstract or `closed` class is
// a compile error." The `closed` half is new with the modifier: through v0.241
// the same class was spelled `sealed abstract` and was rejected by the
// `abstract` half.

module M; closed class Shape public abstract method Area(): int; end Shape; class C public static method Main(): int using var s := new Shape(); return s.Area(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot instantiate `closed` class `Shape` with `new`
// CHECK-SAME: (§12.5)
