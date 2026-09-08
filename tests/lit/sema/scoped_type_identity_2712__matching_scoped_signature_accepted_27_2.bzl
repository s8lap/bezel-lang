// Ported from tests/sema/scoped_type_identity_2712_test.cpp
//   (BEZEL_TEST scoped_type_identity_2712, matching_scoped_signature_accepted_27_2).
//
// §27.2 control: the MATCHING scoped-position assignment is accepted — the
// identity rule forbids conversion, not the type itself.
//
// `Take` returns `owned C` for the same §28.14 v0.180 (#1606) reason as
// above.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Take(scoped f: () -> ()): owned C f(); return new C(); end Take; public static method Main(): int var g: (scoped () -> ()) -> owned C := C.Take; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
