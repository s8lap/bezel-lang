// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, trivial_ctor_eq_accepted_770).
//
// §9.2 (#770 regression guard): a TRIVIAL constructor — only property-
// initialization assignments (`X := x;` / `this.Y := y;`) — does NOT disqualify
// auto-equality; `=` remains available. (Must not over-reject the common form.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pt2 public property X: int; public property Y: int; public constructor(x: int, y: int) X := x; this.Y := y; end constructor; end Pt2; class C public method M() var a := new Pt2(1, 2); var b := new Pt2(1, 2); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
