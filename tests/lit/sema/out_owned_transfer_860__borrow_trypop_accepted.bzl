// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, borrow_trypop_accepted).
//
// §28.3 / §28.11 / #860 (THE BORROW-FORM GUARDRAIL): `Stack<int>.TryPop(out
// item)` — the borrowing form, no `owned`, value-typed — has NO discharge
// obligation on `item`. Dropping it must be ACCEPTED. An over-broad fix keying
// on the call-site/`TryPop` name alone would wrongly reject this.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do const s: Stack<int> := [7]; var item: int? := null; var ok := s.TryPop(out item); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
