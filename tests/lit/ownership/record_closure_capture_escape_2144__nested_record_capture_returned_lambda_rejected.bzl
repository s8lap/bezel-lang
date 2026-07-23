// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 (v0.220, issue #2144): NESTED record-in-record — the captured record
// `o` carries its borrow only TRANSITIVELY (`Outer2.H : Holder`, a borrow-
// bearing record field → §9.7 transitivity), and its provenance to `local`
// resolves through the intermediate record local `h` (the carried-source DFS
// follows record-local hops). The escaping capture of `o` is rejected exactly
// as a direct span-field record capture.

module M; record Holder(S: ArraySpan<int>); record Outer2(H: Holder, N: int); class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); var o := new Outer2(h, 1); return lambda(): int => o.H.S[0] + o.N; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `o`, a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end — a borrow-bearing record may not be captured by a closure that escapes its source's scope
// CHECK-SAME: (§9.7 closure-capture rule
