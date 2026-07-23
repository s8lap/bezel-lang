// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #2127): the INDIRECT span-field-record escape —
// bind the record to a local first, then return the bare local. Once `Holder`
// is classified borrow-bearing (the #2127 fix), collectRecordBorrowSource
// records the holder's provenance to `local`, and checkIndirectEscapeOnReturn
// rejects returning the bare holder whose carried source is a scope-bound owned
// local disposed at scope end. Before the fix the holder was never classified
// borrow-bearing, so no source was recorded and this compiled + SEGFAULTED.

module M; record Holder(S: ArraySpan<int>); class C public static method Leak(): Holder var local := new Array<int>(); local.Add(7); var s := local.AsSpan(); var h := new Holder(s); return h; end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned `h` is a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
