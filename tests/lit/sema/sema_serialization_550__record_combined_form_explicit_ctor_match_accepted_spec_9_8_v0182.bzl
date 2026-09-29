// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, record_combined_form_explicit_ctor_match_accepted_spec_9_8_v0182).
//
// FLIPPED for fix #5132 (§9.3 / §11.1 / §22.6, v0.308). The v0.182 / v0.307
// form of the test was a CLEAN-COMPILE accept: a `record Vec2(X, Y):` with
// a `public constructor(x, y)` (differing from the auto-generated primary
// only in the case of the parameter names) was admitted by the §9.3
// clash check under the v0.307 narrow predicate (equal arity AND equal
// case-sensitive names). The v0.308 ruling (#4775 Q1/Q3/Q4 close) is
// that "same parameter list" is §22.6 overload identity, and parameter
// names do not distinguish; so this exact shape is now REJECTED.
//
// Clean-compile REJECT control: bezelc -c must exit non-zero with the
// §9.3 redeclares-primary diagnostic.
//
// RUN: not %bezelc -c -o %t.o %s 2>&1 | FileCheck --check-prefix=DIAG --allow-empty %s

module M; record Vec2(X: int, Y: int): ISerializable, IDeserializable public constructor(x: int, y: int) X := x + 100; Y := y + 100; end constructor; end Vec2;

// DIAG: body constructor redeclares the primary constructor
// DIAG-SAME: (§9.3, v0.308)
