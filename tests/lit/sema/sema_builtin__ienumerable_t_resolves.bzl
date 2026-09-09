// Ported from tests/sema/builtin_types_test.cpp
//   (BEZEL_TEST sema_builtin, ienumerable_t_resolves).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public _x: IEnumerable<int>?; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
