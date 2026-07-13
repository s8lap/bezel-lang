// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, value_typed_out_owned_not_rejected).
//
// §28.3 / §28.11 / #860 (VALUE-TYPED GUARDRAIL): inline `out var item` on a
// value-typed `Stack<int>.TryPop(out item: int)` — `item` is an `int` with no
// disposal obligation — must NOT be over-rejected. The ownership checker must
// not assign an Owned discharge obligation to value-type out-var locals.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var s := new Stack<int>(); s.Push(7); var ok := s.TryPop(out var item); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
