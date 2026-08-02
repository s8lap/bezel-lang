// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): a mutable static
// FIELD of record type is a compile error — a multi-word record static's
// torn cross-domain read could fabricate a value violating the record's
// own invariants. Previously silently admitted with no diagnostic
// (#1393's "record types" under-enforcement).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; record Point(X: int, Y: int); class App public static _origin: Point := new Point(0, 0); public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_origin` has type `Point`, which §22.12 rule 2 does not admit as a mutable static — a multi-word record static's torn cross-domain read can fabricate a value violating the record's own invariants; use a `Mutex`/`Locked<T>` cell instead
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
