// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, matching_property_override_ok).
//
// §12.1 / #1614: a matching property override (same type) compiles.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual property Name: string => "base"; end Base; class D : Base public override property Name: string => "d"; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
