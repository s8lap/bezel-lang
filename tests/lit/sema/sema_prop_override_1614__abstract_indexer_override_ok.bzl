// Ported from tests/sema/property_override_signature_1614_test.cpp
//   (BEZEL_TEST sema_prop_override_1614, abstract_indexer_override_ok).
//
// §23 / §12.5 / #1614: an abstract indexer shorthand overridden by a matching
// indexer compiles (the §12.5 abstract-getter idiom for indexers).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class Base public abstract property [i: int]: int; end Base; class D : Base public override property [i: int]: int => i; end D;

// NODIAG-NOT: error
// NODIAG-NOT: §
