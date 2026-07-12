// Ported from tests/sema/bare_throw_test.cpp
//   (BEZEL_TEST sema_throw, throw_with_value_in_method_body_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool) throw new Exception(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
