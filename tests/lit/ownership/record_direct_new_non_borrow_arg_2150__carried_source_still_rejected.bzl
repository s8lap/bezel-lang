// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (issue #2150, escape-still-rooted GUARD — leg (b)
// transitive): the precision fix skips only NON-borrow-carrying positional
// args; the borrow-carrying span-field arg keeps both legs (a) and (b). This
// guard pins leg (b) — the carried-source chain (carriedEscapeBoundSourceLiveAt)
// — for the case the #2127 reject was written for: a borrow-bearing SPAN VAR
// (`var sp := s.AsSpan()`) passed to the span field, with a value-typed int
// field following. The span var's root is not itself owned, so leg (a) misses
// it; leg (b) follows sp -> s (escape-bound) and must still report.

module M; record Holder(S: ArraySpan<int>, N: int); class C public static method Leak(p: Array<int>): Holder var s := new Array<int>(); s.Add(7); var sp := s.AsSpan(); return new Holder(sp, p.Count); end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned borrow-bearing record carries a borrow of `s` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
