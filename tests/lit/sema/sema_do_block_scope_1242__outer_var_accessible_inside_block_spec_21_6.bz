// Ported from tests/sema/do_end_block_scope_1242_test.cpp
//   (BEZEL_TEST sema_do_block_scope_1242, outer_var_accessible_inside_block_spec_21_6).
//
// §21.6: an OUTER-scope var read inside a do…end block must still
// resolve (the block is transparent to the enclosing scope upward).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var outer := 10; do var inner := outer + 1; end; return outer; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
