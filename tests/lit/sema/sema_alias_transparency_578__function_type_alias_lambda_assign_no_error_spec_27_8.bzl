// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, function_type_alias_lambda_assign_no_error_spec_27_8).
//
// ---------------------------------------------------------------------------
// 6b §27.8 — function-type alias lambda assignment must not be an error.
// Before fix: "initializer not implicitly convertible to the target type".
// After fix: no error; the alias unfolds to (int)->bool before the check.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Pred := (int) -> bool; class C public method M() var isEven: Pred := (x) => x mod 2 = 0; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
