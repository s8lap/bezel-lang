// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/do_end_block_scope_1242_test.cpp
//   (BEZEL_TEST sema_do_block_scope_1242, outer_block_var_not_visible_after_outer_end_spec_21_6).
//
// §21.6: a var from an outer do-block is not visible after the outer
// end even when the outer block itself surrounds an inner block.

module M; class C public static method Main(): int do var x := 1; do var y := 2; end; end; return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `x`
