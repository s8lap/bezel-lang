// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §11.7 / A.3 (v0.242, #3052): `closed` is defined for a CLASS ("A class
// declared with the **`closed`** modifier is a **closed abstract class**").
// A.3's `Modifier*` production is shared by every type declaration, so
// `closed record` parses; it has no meaning and is rejected semantically —
// the same over-admit-and-reject split the grammar's own note describes for
// `static class C(args)`, and the same reading already applied to
// `abstract record` (#2431).
//
// Recorded interpretation (CLAUDE.md rule 4): the alternative — letting the
// modifier ride along inertly — would make `new R()` on a record fail with a
// diagnostic about a class rule.

module M; closed record Point(X: int, Y: int);

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Point` may not be `closed`
// CHECK-SAME: (§11.7)
