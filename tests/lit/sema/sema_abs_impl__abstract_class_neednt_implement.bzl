// Ported from tests/sema/abstract_impl_test.cpp
//   (BEZEL_TEST sema_abs_impl, abstract_class_neednt_implement).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface IDoer method Do(); end IDoer; abstract class Partial : IDoer end Partial;

// NODIAG-NOT: error
// NODIAG-NOT: §
