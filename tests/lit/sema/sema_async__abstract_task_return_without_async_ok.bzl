// Ported from tests/sema/async_abstract_test.cpp
//   (BEZEL_TEST sema_async, abstract_task_return_without_async_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class C public abstract method F(): Task; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
