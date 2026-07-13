// Ported from tests/sema/nullable_field_narrowing_862_test.cpp
//   (BEZEL_TEST nullable_field_narrowing_862, ownedbuffer_index_bang_member_accepted_1161).
//
// §7.4 (#1161, force-unwrap): `buf[0]!.member` narrows the index result to
// non-null — it must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public v: int; public constructor(x: int) this.v := x; end constructor; end Cell; class C public static method Main(): int using var buf := new OwningBuffer<Cell>(2); buf[0] := new Cell(7); return buf[0]!.v; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
