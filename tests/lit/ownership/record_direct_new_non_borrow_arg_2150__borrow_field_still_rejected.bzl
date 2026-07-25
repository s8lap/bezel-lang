// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 (issue #2150, escape-still-rooted GUARD): the direct-`new` precision fix
// skips a positional arg whose matched field is non-borrow-carrying (a value-
// typed field holds a VALUE, not a borrow). This guard pins that the fix does
// NOT under-root a genuine escape: when the SAME constructor's borrow-carrying
// field (the span) is sourced from a scope-bound owned local, the escape must
// still be rejected. The record mirrors the ACCEPT case in
// record_direct_new_non_borrow_arg_2150_test.cpp (Holder with a span field PLUS
// an int field); only the span's source differs — `s.AsSpan()` (escape-bound)
// here vs `p.AsSpan()` (param) there.
//
// Legs (a) and (b) of checkRecordBorrowEscapesOnReturn's direct-`new` branch
// (Ownership_Checks.cpp ~2915/2928) must still fire for the borrow-carrying
// span-field position even when a non-borrow-carrying int field follows it.

module M; record Holder(S: ArraySpan<int>, N: int); class C public static method Leak(p: Array<int>): Holder var s := new Array<int>(); s.Add(7); return new Holder(s.AsSpan(), p.Count); end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned borrow-bearing record carries a borrow of `s` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
