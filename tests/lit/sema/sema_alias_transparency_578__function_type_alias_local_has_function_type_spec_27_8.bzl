// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, function_type_alias_local_has_function_type_spec_27_8).
//
// 6b §27.8 — a Pred local carries a Function declType after the fix.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Pred := (int) -> bool; class C public method M() var isEven: Pred := lambda(x) => x mod 2 = 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
