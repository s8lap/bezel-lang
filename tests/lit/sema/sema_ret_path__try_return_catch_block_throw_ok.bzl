// Ported from tests/sema/definite_return_test.cpp
//   (BEZEL_TEST sema_ret_path, try_return_catch_block_throw_ok).
//
// The block-form throwing catch arm was already accepted; keep it covered so
// the two forms stay in agreement.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(): int try return 1; catch ArgumentException => do throw new InvalidOperationException("x"); end; end; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
