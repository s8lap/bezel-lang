// Ported from tests/sema/sealed_class_test.cpp
//   (BEZEL_TEST sema_sealed, closed_class_same_module_plain_subclass_ok_spec_11_7).
//
// §11.7: a non-sealed subclass of a `closed` base, same module, is
// equally allowed — the closed-abstract base restricts only cross-module
// derivation, not the subclass's own modifiers.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; closed class Base public abstract method V(): int; end Base; class Derived : Base public override method V(): int => 1; end Derived;

// NODIAG-NOT: error
// NODIAG-NOT: §
