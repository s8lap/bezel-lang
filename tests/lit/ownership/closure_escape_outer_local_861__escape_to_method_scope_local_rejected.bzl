// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/closure_escape_outer_local_861_test.cpp
//   (BEZEL_TEST closure_escape_outer_local_861, escape_to_method_scope_local_rejected).
//
// §28.3 / §28.9: `f` is a method-scope local; `r` is a using-var in an inner
// do-end. `f := () => r.V` lets the closure (held by `f`) outlive `r`. Rejected.
// This is the exact #861 repro that compiled and SEGFAULTED before the fix.

module M; class R public V: int; public constructor(v: int) V := v; end; destructor V := 0; end; end R; class C public static method Main(): int var f: () -> int := () => -1; do using var rr := new R(55); f := () => rr.V; end; return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure capturing `rr` is assigned to `f`, which outlives it — `rr` is an owned scope-bound local disposed at the end of its block, but `f` lives in an enclosing block, so calling the closure after the inner block exits reads a disposed value
// CHECK-SAME: (§28.3 / §28.9; move an owned value into the closure, or keep `rr` in a block that outlives `f`)
