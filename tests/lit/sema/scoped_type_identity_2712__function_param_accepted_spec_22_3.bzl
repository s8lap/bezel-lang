// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, function_param_accepted_spec_22_3).
//
// §22.3 control: `scoped` on a function-typed parameter is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(scoped f: () -> ()) f(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
