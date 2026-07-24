// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.4 (v0.220, issue #2180, route 1): a borrow-bearing CHOICE LOCAL
// captured by a closure returned from the method. §35.4: a borrow-bearing
// choice "counts as a live local borrow of every carried borrow's source" and
// is subject to §9.7's closure-capture bullet with "record" read as "record or
// choice". `b := Box.Got(hh)` carries `hh`'s borrow of `local`; before the fix
// the capture-escape legs gated on isBorrowBearingRecordOrSpan (false for a
// choice) so the choice CARRIER slipped through and dangled at runtime once
// `local` auto-dropped (§28.3). `Sink` reads the carried span so the capture is
// a real dereference route. // per §9.7, §35.4 (v0.220)

module M; record Holder(S: ArraySpan<int>); choice Box Got(h: Holder), Nothing; end Box; class C public static method Sink(v: Box): int var acc := 0; case v of Got(var y) => do acc := y.S[0]; end, Nothing => do end end; return acc; end Sink; public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var hh := new Holder(local.AsSpan()); var b := Box.Got(hh); return lambda(): int => C.Sink(b); end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `b`, a borrow-bearing choice carrying a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: §35.4
