// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, indexer_param_widening_override_ok).
//
// §23 / §12.1 (v0.181, #1560) / #1614: an indexer's index parameter may
// WIDEN the base's `T` to `T?` — it is a parameter (an input), so the same
// contravariant nullability allowance §12.1 grants method parameters applies.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual property [s: string]: int => 0; end Base; class D : Base public override property [s: string?]: int => 0; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
