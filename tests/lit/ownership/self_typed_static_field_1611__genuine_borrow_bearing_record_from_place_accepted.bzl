// Ported from tests/ownership/self_typed_static_field_1611_test.cpp
//   (BEZEL_TEST self_typed_static_field_1611, genuine_borrow_bearing_record_from_place_accepted).
//
// The same field bound to a named local/parameter (a place whose scope
// outlives the record) stays ACCEPTED — the positive control pinning that
// #1611's fix leaves the ordinary (non-cyclic) borrow-bearing path fully
// intact.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Bag(Items: Array<int>); class C public method Run(xs: Array<int>) var b := new Bag(xs); end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
