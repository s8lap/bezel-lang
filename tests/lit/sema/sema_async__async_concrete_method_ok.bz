// Ported from tests/sema/async_abstract_test.cpp
//   (BEZEL_TEST sema_async, async_concrete_method_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public async method F(): Task return; end F; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
