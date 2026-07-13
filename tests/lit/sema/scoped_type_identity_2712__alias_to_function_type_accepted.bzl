// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, alias_to_function_type_accepted).
//
// §22.3 (v0.175): the function-typed check is ALIAS-TRANSPARENT — a `scoped`
// parameter whose type is an alias resolving to a function type is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Callback := () -> (); class C public method M(scoped f: Callback) f(); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
