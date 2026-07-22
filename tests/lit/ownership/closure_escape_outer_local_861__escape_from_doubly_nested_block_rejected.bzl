// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/closure_escape_outer_local_861_test.cpp
//   (BEZEL_TEST closure_escape_outer_local_861, escape_from_doubly_nested_block_rejected).
//
// §28.3 / §28.9: a deeper nesting — `f` at method scope, `rr` two blocks in. The
// target is still shallower than the source, so the escape is rejected.

module M; class R public V: int; public constructor(v: int) V := v; end; destructor V := 0; end; end R; class C public static method Main(): int var f: () -> int := lambda() => -1; do do using var rr := new R(7); f := lambda() => rr.V; end; end; return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure capturing `rr` is assigned to `f`, which outlives it — `rr` is an owned scope-bound local disposed at the end of its block, but `f` lives in an enclosing block, so calling the closure after the inner block exits reads a disposed value
// CHECK-SAME: (§28.3 / §28.9; move an owned value into the closure, or keep `rr` in a block that outlives `f`)
