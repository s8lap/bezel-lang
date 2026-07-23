// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #2127): a record with a builtin-SPAN field is a
// borrow-bearing record by transitivity — a span (§4.14/§4.15) is a borrow of
// its backing Array/OwningBuffer, and §9.7 makes a record holding a borrow-
// carrying field borrow-bearing. Before the fix, isBorrowBearingRecordImpl's
// field walk bailed on the span field (null/value-builtin declaration, no user
// Record decl), so `Holder` was NOT classified borrow-bearing and this DIRECT
// escape compiled and SEGFAULTED (dangling span read once `local` auto-drops).
// This is the issue's exact direct repro; the pinning reject.

module M; record Holder(S: ArraySpan<int>); class C public static method Leak(): Holder var local := new Array<int>(); local.Add(7); return new Holder(local.AsSpan()); end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned borrow-bearing record carries a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
