// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §28.9 (v0.220, issue #2181): the `for`-nested sibling of the `if`
// repro — a borrow-bearing record `h` captured only UNDER a `for` loop inside
// a block-bodied lambda that is returned. The loop body is a control-flow
// statement of the lambda block the pre-fix bounded slice never walked. The
// range `0..1` iterates a lambda-local index, so the ONLY method-scope capture
// is `h` (and, through it, `local`). // per §9.7 (v0.220), §28.3

module M; record Holder(S: ArraySpan<int>); class C public static method MakeLeak(): () -> int var local := new Array<int>(); local.Add(7); var h := new Holder(local.AsSpan()); return lambda(): int => do for var k in 0..1 do return h.S[0]; end; return 0; end; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `h`, a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 closure-capture rule
