// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #1966): the slice-hop indirect span escape. A
// derived view `var t := s.Slice(0, 1);` inherits `s`'s provenance to `local`
// (identifierRoot peels the `.Slice(...)` call to `s`; the transitive walk
// reaches `local`). Returning the sliced span dangles the view once `local` is
// auto-dropped (UAF before the fix). Note: `t.Slice`-through-`s` is a KEPT
// self-derivation for consume-freeze, but RETURNING it still escapes `local`.

module M; class C public static method Leak(): ArraySpan<int> var local := new Array<int>(); local.Add(7); local.Add(9); var s := local.AsSpan(); var t := s.Slice(0, 1); return t; end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned `t` is a span viewing `local` whose backing storage is disposed at scope end
// CHECK-SAME: (§9.7 borrow-lifetime rule
