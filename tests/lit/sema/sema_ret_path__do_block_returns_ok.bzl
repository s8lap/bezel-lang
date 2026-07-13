// Ported from tests/sema/definite_return_test.cpp
//   (BEZEL_TEST sema_ret_path, do_block_returns_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(): int do return 1; end; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
