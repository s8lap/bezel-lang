// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.4 / §28.3 / §28.9 (v0.220, issue #2180, route 1 via the #861
// outer-local store): a borrow-bearing CHOICE local captured by a closure that
// is ASSIGNED to an outer-scope local `f`. `f` lives at method scope; `b` (the
// borrow-bearing choice) and its carried source `local` live in the inner
// do-end block. The closure capturing `b` inherits its carried source through
// borrowSourceForExpr Shape 3 (now widened to borrow-bearing choices), so the
// existing #861 outer-local store check rejects the assignment — `f()` after
// the block would read the dangling span. // per §9.7, §35.4, §28.3, §28.9

module M; record Holder(S: ArraySpan<int>); choice Box Got(h: Holder), Nothing; end Box; class C public static method Sink(v: Box): int var acc := 0; case v of Got(var y) => do acc := y.S[0]; end, Nothing => do end end; return acc; end Sink; public static method Main(): int var f: () -> int := lambda() => -1; do var local := new Array<int>(); local.Add(7); var hh := new Holder(local.AsSpan()); var b := Box.Got(hh); f := lambda(): int => C.Sink(b); end; return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure capturing `local` is assigned to `f`, which outlives it
// CHECK-SAME: (§28.3 / §28.9
