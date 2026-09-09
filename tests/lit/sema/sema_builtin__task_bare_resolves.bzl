// Ported from tests/sema/builtin_types_test.cpp
//   (BEZEL_TEST sema_builtin, task_bare_resolves).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: Task?; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
