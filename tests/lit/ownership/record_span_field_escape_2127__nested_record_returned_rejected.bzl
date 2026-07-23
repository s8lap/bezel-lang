// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 transitivity / §4.14 / §4.15 (issue #2127): a NESTED span-field record —
// `Outer` holds a `Holder`, and `Holder` holds a span, so `Outer` is borrow-
// bearing by transitivity (§9.7: "Its type is another borrow-bearing record …
// (transitivity)"). Returning `new Outer(h)` where `h` views a scope-bound
// owned local dangles once `local` auto-drops. The direct-`new` branch follows
// the carried-source chain through the nested holder argument `h` (its recorded
// source is `local`). Before the fix `Outer` was never classified borrow-
// bearing (the span leaf was missed), so this compiled + SEGFAULTED. // per §9.7

module M; record Holder(S: ArraySpan<int>); record Outer(H: Holder); class C public static method Leak(): Outer var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); return new Outer(h); end Leak; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned borrow-bearing record carries a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
