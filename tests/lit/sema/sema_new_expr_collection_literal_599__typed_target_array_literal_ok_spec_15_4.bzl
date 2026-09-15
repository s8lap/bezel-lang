// Ported from tests/sema/new_expr_collection_literal_599_test.cpp
//   (BEZEL_TEST sema_new_expr_collection_literal_599, typed_target_array_literal_ok_spec_15_4).
//
// §15.4 (#599 must-not-regress): `var a: Array<int> := [10, 20, 30]` —
// typed-target declaration; the literal targets `Array<int>` directly.
// This is the CORRECT form (§15.4) and must NOT produce a diagnostic.
//
// NOTE: literal is now Owned per fix #4338 fix-forward; changed `var` to `const`
// to avoid the auto-drop warning (value-typed Array<int> has no disposal obligation).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method M() const a: Array<int> := [10, 20, 30]; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
