// Ported from tests/sema/do_end_block_scope_1242_test.cpp
//   (BEZEL_TEST sema_do_block_scope_1242, outer_var_writable_inside_block_spec_21_6).
//
// §21.6: writing to an outer-scope var from inside a do…end block is
// permitted (upward mutation through the scope chain).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var sum := 0; do sum := sum + 5; end; return sum; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
