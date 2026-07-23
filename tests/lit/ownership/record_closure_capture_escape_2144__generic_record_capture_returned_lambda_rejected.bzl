// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §30.1 (v0.220, issues #2144 + #2145 composed): a GENERIC record
// instantiated with a borrow-carrying type argument (`Wrap<ArraySpan<int>>`,
// classified borrow-bearing at the instantiation site per #2145) captured by
// an escaping closure (#2144). Both fixes must compose: the instantiation-
// site classification makes `w` a borrow-bearing record local, and the §9.7
// closure bullet then rejects the escaping capture exactly as for a concrete
// span-field record.

module M; record Wrap<T>(V: T); class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var w := new Wrap<ArraySpan<int>>(local.AsSpan()); return lambda(): int => w.V[0]; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `w`, a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end — a borrow-bearing record may not be captured by a closure that escapes its source's scope
// CHECK-SAME: (§9.7 closure-capture rule
