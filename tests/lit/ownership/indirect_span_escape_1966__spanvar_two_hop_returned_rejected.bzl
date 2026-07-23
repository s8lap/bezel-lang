// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #1966): the two-hop indirect span escape. The
// span's provenance to `local` flows through a second bare-alias binding
// (`var t := s;`); carriedEscapeBoundSourceLiveAt resolves it transitively over
// recordBorrowLocalSource_ (t -> s -> local). Returning `t` dangles the view
// once `local` is auto-dropped (UAF before the fix).

module M; class C public static method Leak(): ArraySpan<int> var local := new Array<int>(); local.Add(7); var s := local.AsSpan(); var t := s; return t; end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned `t` is a span viewing `local` whose backing storage is disposed at scope end
// CHECK-SAME: (§9.7 borrow-lifetime rule
