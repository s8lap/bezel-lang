// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/do_end_block_scope_1242_test.cpp
//   (BEZEL_TEST sema_do_block_scope_1242, nested_blocks_each_scope_spec_21_6).
//
// §21.6: nested do…end blocks each introduce their own scope; the
// inner var is not visible in the outer block after the inner end.

module M; class C public static method Main(): int do do var inner := 7; end; return inner; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `inner`
