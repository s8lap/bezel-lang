// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §4.14 / §4.15 (v0.220, issue #2181): a BARE builtin-span local
// captured only UNDER a `while` inside a block-bodied lambda that is returned.
// The `while` body is a control-flow statement of the lambda block that the
// pre-fix collectIdentifiersInStmt bounded slice never walked, so the span
// capture was invisible to the §9.7 span leg and the returned closure read the
// dangling backing store once `local` auto-dropped (§28.3). // per §9.7, §4.15

module M; class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var s := local.AsSpan(); return lambda(): int => do var i := 0; while i < 1 do return s[0]; end; return 0; end; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `s`, a span viewing `local` whose backing storage is disposed at scope end
// CHECK-SAME: (§9.7 closure-capture rule
