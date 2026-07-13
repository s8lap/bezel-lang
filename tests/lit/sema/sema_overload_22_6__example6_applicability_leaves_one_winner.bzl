// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, example6_applicability_leaves_one_winner).
//
// §22.6 Example 6 — applicability filtering leaves one winner.
// byte -> sbyte is NOT implicit (signed/unsigned same-or-narrower width);
// byte -> ushort IS implicit. Process(ushort) is the only applicable
// candidate, so the call resolves with no ambiguity. Process(sbyte) is
// first in source; a first-wins resolver would (wrongly) pick it.
// Process(ushort) returns int, Process(sbyte) returns bool — binding to
// `var x: int` confirms the ushort overload was selected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Handler public method Process(n: sbyte): bool => true; public method Process(n: ushort): int => 1; public method Use(val: byte) var x: int := this.Process(val); end Use; end Handler;

// NODIAG-NOT: error
// NODIAG-NOT: §
