// Ported from tests/sema/override_signature_test.cpp
//   (BEZEL_TEST sema_override, widening_param_nullability_override_ok).
//
// §12.1 (v0.181, #1560): an override may widen a parameter from the base's
// `T` (string) to `T?` (string?) — the same contravariant nullability
// allowance §11.2 grants interface implementations ("an implementation may
// declare a parameter as `T?` where the interface requires `T`").
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual method F(a: string) _ := 1; end F; end Base; class D : Base public override method F(a: string?) _ := 2; end F; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
