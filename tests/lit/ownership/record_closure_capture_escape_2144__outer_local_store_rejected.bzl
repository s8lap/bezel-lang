// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §28.3 / §28.9 (v0.220, issue #2144): the #861 ASSIGNED-TO-OUTER-LOCAL
// escape route, with the capture being a borrow-bearing record instead of the
// owned local itself. `f` lives at method scope; `h` (span-field record) and
// its carried source `local` live in the inner do-end block. `f := lambda …`
// lets the closure — and through the captured record, a borrow of `local` —
// outlive the block that disposes `local` (§28.3 auto-drop), so `f()` after
// the block reads the dangling span. The closure's holder inherits the
// record's carried source (borrowSourceForExpr Shape 3), and the existing
// #861 outer-local store check rejects the assignment.

module M; record Holder(S: ArraySpan<int>); class C public static method Main(): int var f: () -> int := lambda() => -1; do var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); f := lambda(): int => h.S[0]; end; return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure capturing `local` is assigned to `f`, which outlives it
// CHECK-SAME: (§28.3 / §28.9
