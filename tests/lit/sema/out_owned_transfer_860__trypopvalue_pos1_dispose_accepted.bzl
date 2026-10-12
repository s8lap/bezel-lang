// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, trypopvalue_pos1_dispose_accepted).
//
// §28.3 / §28.11 / #860: TryPopValue at arg 1 with a null-safe dispose — the
// key (arg 0) is a borrow and only arg 1 is the out-owned transfer. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public _id: int := 0; public constructor(id: int) _id := id; end constructor; destructor end destructor; end Cell; class C public static method Main(): int do using var d := new OwningDictionary<string, Cell>(); d.Add("a", (new Cell(1))); var ok := d.TryPopValue("a", out var v); v?.Dispose(); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
