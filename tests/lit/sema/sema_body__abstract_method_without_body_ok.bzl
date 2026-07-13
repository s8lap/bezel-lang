// Ported from tests/sema/method_body_presence_test.cpp
//   (BEZEL_TEST sema_body, abstract_method_without_body_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class C public abstract method F(): int; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
