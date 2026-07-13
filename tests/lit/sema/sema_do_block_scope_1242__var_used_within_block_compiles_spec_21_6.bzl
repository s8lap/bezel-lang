// Ported from tests/sema/do_end_block_scope_1242_test.cpp
//   (BEZEL_TEST sema_do_block_scope_1242, var_used_within_block_compiles_spec_21_6).
//
// §21.6: a var used WITHIN the same do…end block must still resolve
// (no over-rejection).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do var x := 1; var y := x + 1; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
