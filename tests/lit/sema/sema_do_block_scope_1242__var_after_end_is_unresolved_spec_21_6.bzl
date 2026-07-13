// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/do_end_block_scope_1242_test.cpp
//   (BEZEL_TEST sema_do_block_scope_1242, var_after_end_is_unresolved_spec_21_6).
//
// §21.6 Facet A: a var declared inside a do…end block is not visible
// after the matching end — must produce a compile error (#1242).

module M; class C public static method Main(): int do var secret := 42; end; return secret; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `secret`
