// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/hiding_680_primary_ctor_param_test.cpp
//   (BEZEL_TEST sema_680_primary_ctor_hiding, body_field_not_affected_by_exemption).
//
// §12.2 / §11.1 / #680: a record's positional field STILL participates in
// hiding — record fields ARE public members, unlike class primary-ctor params.
// (Record-record hiding is disallowed; class-derived-from-record with same
// field name is also disallowed.)
// Here we test that a BODY-DECLARED field in a derived class whose name
// matches a base class's explicit body-declared field still diagnoses.
//
// `Q` uses an EMPTY primary-constructor parameter list `()` to legally
// thread `0` to `P`'s primary ctor (fix #2455, §11.1/§22.10/§12.8, v0.235):
// the bare form `class Q : P(0)` (no primary ctor on Q at all) is no longer
// valid Bezel — a header base-constructor argument list with no primary
// constructor of Q's own to feed has no defined destination, and is now a
// compile-time reject rather than the silently-dropped-args bug #2455 fixed.
// `Q()` is the "add an empty primary-constructor parameter list" remedy
// named in that fix's own diagnostic; it does not affect the hiding check
// under test here (§12.2), which concerns the explicit body fields only.

module M; class P(x: int) public _x: int := x; end P; class Q() : P(0) public _x: int; end Q;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `_x` hides an inherited member of the same name; hiding is forbidden — use `override` for a virtual base member, or choose a different name
// CHECK-SAME: (§12.2)
