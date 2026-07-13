// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, alias_of_double_plus_no_error_spec_27_8).
//
// ---------------------------------------------------------------------------
// 6a §27.8 — primitive alias operators: Meters + Meters must not be an error.
// Before fix: "operator '+' is not defined for type '?'".
// After fix: no error; the alias unfolds to double before operator lookup.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Meters := double; class C public static method M(a: Meters, b: Meters): Meters return a + b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
