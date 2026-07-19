// Ported from tests/sema/variance_position_test.cpp
//   (BEZEL_TEST sema_variance_marker_scope, spec_11_8_static_method_invariant_accepted).
//
// §11.8 regression: an interface with a static member but an INVARIANT type
// parameter (no `in`/`out`) is unaffected — only the marker is forbidden.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
// v0.201 (#2048): the producer static member is spelled `owned T` per the
// declaration-site static-return rule.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; interface INumber3<T> where T: INumber3<T> static method Zero(): owned T; end INumber3;

// NODIAG-NOT: error
// NODIAG-NOT: §
