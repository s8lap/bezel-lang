// Ported from tests/sema/base_abstract_call_874_test.cpp
//   (BEZEL_TEST sema_base_abstract_call_874, virtual_this_call_accepted).
//
// A virtual dispatch via `this` to a method whose origin is abstract (but
// which the concrete class overrides) must still be accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; abstract class Shape public abstract method Area(): int; end Shape; class Rectangle : Shape public override method Area(): int => 12; public method Test(): int return this.Area(); end Test; end Rectangle;

// NODIAG-NOT: error
// NODIAG-NOT: §
