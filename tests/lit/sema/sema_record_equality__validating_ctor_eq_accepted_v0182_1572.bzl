// Ported from tests/sema/record_equality_eligibility_test.cpp
//   (BEZEL_TEST sema_record_equality, validating_ctor_eq_accepted_v0182_1572).
//
// §9.2 v0.182 (#1572): a VALIDATING constructor — the spec's own motivating
// example ("Width > 0 and Height > 0"), which THROWS on invalid input rather
// than merely clamping — is likewise fully compatible with auto-equality.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Rectangle public property Width: decimal; public property Height: decimal; public constructor(width: decimal, height: decimal) if width <= 0 or height <= 0 then throw new Exception("bad dims"); end; Width := width; Height := height; end constructor; end Rectangle; class C public method M() var a := new Rectangle(3, 4); var b := new Rectangle(3, 4); _ := a = b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
