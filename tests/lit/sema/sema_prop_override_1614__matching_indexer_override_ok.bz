// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, matching_indexer_override_ok).
//
// §23 / §12.1 / #1614: a matching indexer override compiles.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Base public virtual property [i: int]: int => 0; end Base; class D : Base public override property [i: int]: int => i; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
