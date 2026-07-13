// Ported from tests/sema/nullable_field_narrowing_862_test.cpp
//   (BEZEL_TEST nullable_field_narrowing_862, nonnullable_record_index_member_accepted_1161).
//
// §7.4 (#1161, no over-reject): a NON-nullable index result must still admit a
// bare `.member`. A record-element collection is value-typed, so its indexer is
// non-nullable — `buf[0].field` is fine and must NOT be flagged.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pt(x: int, y: int); class C public static method Main(): int using var buf := new OwningBuffer<Pt>(2); buf[0] := new Pt(3, 4); return buf[0].x; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
