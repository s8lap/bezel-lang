// Ported from tests/sema/bare_throw_test.cpp
//   (BEZEL_TEST sema_throw, bare_throw_in_catch_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool) try flag := true; catch var e: Exception => throw; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
