// Ported from tests/ownership/record_from_temporary_1066_test.cpp
//   (BEZEL_TEST record_from_temporary_1066, long_lived_local_accepted).
//
// A named local whose scope outlives the record: `var xs2 := […]; new Bag(xs2);`.
// xs2 is a place; the borrow's source outlives the record. ACCEPT.
//
// NOTE: literal is now Owned per fix #4338 fix-forward; changed `var` to `const`
// to avoid the auto-drop warning (value-typed Array<int> has no disposal obligation).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Bag(Items: Array<int>); record Pt(X: int, Y: int); class C public method Run(xs: Array<int>) const xs2: Array<int> := [1, 2, 3]; var b := new Bag(xs2); end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
