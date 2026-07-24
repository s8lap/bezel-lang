// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.5 (v0.220, issue #2180, route 2): a case-arm payload BINDING of a
// borrow-bearing record (a borrow-bearing choice's payload) captured by a
// closure returned from the method. §35.5: "a pattern binding is a borrow whose
// source is the scrutinee's source" (§19.3) — so `x` carries the choice
// scrutinee `b`'s borrow of `local`. The arm binding is a pattern BindingPattern,
// not a LocalDecl, so before the fix it had no provenance and the §9.7 record
// leg could not resolve its carried source: the program compiled and read the
// dangling span once `local` auto-dropped (§28.3). The issue's exact repro
// (verified on main: ASan SEGV in bz_array_get via lam$0). // per §9.7, §35.5

module M; record Holder(S: ArraySpan<int>); choice Box Got(h: Holder), Nothing; end Box; class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var hh := new Holder(local.AsSpan()); var b := Box.Got(hh); case b of Got(var x) => do return lambda(): int => x.S[0]; end, Nothing => do end end; return lambda(): int => 0; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `x`, a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 closure-capture rule
