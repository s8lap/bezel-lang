// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (v0.220, issue #2144): a BARE builtin-span local
// captured by a returned lambda — a span is a borrow-bearing record without a
// user Record TypeDecl (the #1959/#2127 reading), so §9.7's closure bullet
// ("may not be captured by a closure that escapes its source's scope")
// applies to it directly. Before the fix this compiled and read the dangling
// backing store once `local` auto-dropped (§28.3).

module M; class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var s := local.AsSpan(); return lambda(): int => s[0]; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `s`, a span viewing `local` whose backing storage is disposed at scope end — a span (§4.14/§4.15) is a borrow of its backing Array / OwningBuffer, and a borrow-bearing record may not be captured by a closure that escapes its source's scope
// CHECK-SAME: (§9.7 closure-capture rule
