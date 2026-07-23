// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 (v0.220, issue #2144): "A borrow-bearing record may not be captured by
// a closure that escapes its source's scope." `h` is a borrow-bearing record
// (span field, §9.7 transitivity per #2127) whose carried borrow's source is
// the scope-bound owned local `local`; the returned lambda captures `h` and
// escapes the method, so calling it after `local` auto-drops (§28.3) reads the
// dangling span. This is the issue's exact repro: before the fix it COMPILED
// and SEGFAULTED under the AOT+ASan oracle (SEGV in the span accessor).

module M; record Holder(S: ArraySpan<int>); class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); return lambda(): int => h.S[0]; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `h`, a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end — a borrow-bearing record may not be captured by a closure that escapes its source's scope
// CHECK-SAME: (§9.7 closure-capture rule
