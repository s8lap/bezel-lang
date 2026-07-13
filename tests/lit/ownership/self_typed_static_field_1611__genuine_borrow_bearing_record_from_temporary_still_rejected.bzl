// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/self_typed_static_field_1611_test.cpp
//   (BEZEL_TEST self_typed_static_field_1611, genuine_borrow_bearing_record_from_temporary_still_rejected).
//
// `Bag` (the #1066 repro type, a POSITIONAL record — §9.1 — whose single
// field is class-typed `Array<int>`) is a real borrow-carrying field per
// §9.7, so `isBorrowBearingRecord(Bag)` must still return true and the
// existing #1066 no-temporary-source rule must still fire. This pins that
// the added static-field skip / visited-set guard does not suppress a
// genuine borrow-bearing classification; only the self-/mutually-
// referential STATIC path (which #1611's fix newly excludes) is affected.

module M; record Bag(Items: Array<int>); class C public method Run() var b := new Bag([1, 2, 3]); end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a borrow-bearing record may not be constructed from a temporary bound to its borrow-carrying field `Items` — the temporary's lifetime ends at this construction expression, so the record would immediately outlive that borrow's source (a dangling borrow); bind the source to a named local whose scope outlives the record and pass that
// CHECK-SAME: (§9.7 / §28.3 borrow-lifetime rule)
