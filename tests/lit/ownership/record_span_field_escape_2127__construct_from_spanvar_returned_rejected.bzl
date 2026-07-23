// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #2127): the direct-`new` return whose constructor
// argument is itself a borrow-bearing local — `var s := local.AsSpan(); return
// new Holder(s);`. checkRecordBorrowEscapesOnReturn's direct-`new` branch
// formerly only reported when an arg ROOT was itself a scope-bound owned local
// (`new Holder(local.AsSpan())`, root `local`); here the arg root `s` is a span
// var, not owned, so its CARRIED source (`local`, via recordBorrowLocalSource_)
// went unchecked and this SEGFAULTED. The fix follows the carried-source chain
// (carriedEscapeBoundSourceLiveAt) for such arguments. // per §9.7

module M; record Holder(S: ArraySpan<int>); class C public static method Leak(): Holder var local := new Array<int>(); local.Add(7); var s := local.AsSpan(); return new Holder(s); end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned borrow-bearing record carries a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
