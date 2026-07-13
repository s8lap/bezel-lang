// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/do_end_block_scope_1242_test.cpp
//   (BEZEL_TEST sema_do_block_scope_1242, local_const_after_end_is_unresolved_spec_21_6).
//
// §21.6: a local const declared inside a do…end block also goes out
// of scope at the matching end.

module M; class C public static method Main(): int do const K: int := 99; end; return K; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `K`
