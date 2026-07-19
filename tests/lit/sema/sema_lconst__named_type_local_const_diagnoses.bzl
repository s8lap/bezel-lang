// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Spec §21.1 v0.201 (#2049): a `const` local whose initializer is NOT a
// §17.14 constant expression OR whose declared/inferred type is NOT a
// primitive value type / `string` is a RUNTIME const, not an error. The
// pre-v0.201 "const must have a primitive value type or `string`" reject
// (§22.12's class-level restriction, which the OLD test asserted) is
// superseded for LOCAL const by the §21.1 runtime-const classification.
//
// `const X: T := null;` (T a class type) is admissible as a runtime const
// — a permanently-null binding (§21.1 "Ownership": `null` is admitted; the
// binding is then permanently null).
//
// Ported from tests/sema/local_const_type_test.cpp
//   (BEZEL_TEST sema_lconst, named_type_local_const_diagnoses) — the test
//   was repurposed for the v0.201 classification split (#2049).

module M; class T end T; class C public method M() const X: T := null; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
