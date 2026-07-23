// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #1966): the INDIRECT span borrow-escape — the
// explicitly-deferred residual of #1959. #1959 closed the DIRECT projection
// `return local.AsSpan();`; this closes the one-hop-through-a-local form:
//
//   var s := local.AsSpan();   // span bound to a local var (source: `local`)
//   return s;                  // `local` dies at return -> dangling span -> UAF
//
// A span (§4.14/§4.15) is a borrow of its backing Array / OwningBuffer. Bound
// to a local first, its provenance to `local` lives in recordBorrowLocalSource_
// (collectRecordBorrowSource); the direct-return check consults only the
// returned expression's identifierRoot, so returning the span VAR went
// unchecked. It compiled and SEGFAULTED at runtime (UAF in the span accessor
// once `local` is auto-dropped). This is the issue's exact repro; the pinning
// reject.

module M; class C public static method Leak(): ArraySpan<int> var local := new Array<int>(); local.Add(7); var s := local.AsSpan(); return s; end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned `s` is a span viewing `local` whose backing storage is disposed at scope end
// CHECK-SAME: (§9.7 borrow-lifetime rule
