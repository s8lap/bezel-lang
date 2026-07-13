// Ported from tests/sema/bare_drain_discard_832_test.cpp
//   (BEZEL_TEST bare_drain_discard_832, borrowing_get_accepted).
//
// A borrowing access (Get) returns a borrow, not an owned value → not rejected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Main(): int using var a := new OwningArray<R>(); a.Add(move (new R())); a.Get(0); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
