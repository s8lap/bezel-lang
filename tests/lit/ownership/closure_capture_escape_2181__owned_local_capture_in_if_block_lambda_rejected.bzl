// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 (v0.220, issue #2181): the PRE-EXISTING owned-local capture leg the
// same bounded slice caps. An owned class local `r` captured only UNDER an
// `if` inside a block-bodied lambda that is returned: capturing an owned local
// is a borrow of the enclosing scope, and the returned closure outlives it
// (use-after-dispose once `r` auto-drops, §28.3). This leg predates #2144 —
// the collector never walked the block lambda's control-flow statements, so
// the owned-local capture nested under `if` slipped through §28.3 too, not
// just the #2144 record leg. Widening the collector closes both together. The
// condition `n > 0` uses a lambda-local so ONLY `r` is a method-scope capture.
// // per §28.3 (v0.220)

module M; class Res public method V(): int return 5; end V; end Res; class C public static method Make(): () -> int var r := new Res(); return lambda(): int => do var n := 1; if n > 0 then return r.V(); end; return 0; end; end Make; public static method Main(): int var f := C.Make(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `r` whose source is disposed at scope end
// CHECK-SAME: (§28.3 closure-capture escape
