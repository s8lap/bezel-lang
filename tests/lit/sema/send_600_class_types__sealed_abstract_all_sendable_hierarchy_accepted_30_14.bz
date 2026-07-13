// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, sealed_abstract_all_sendable_hierarchy_accepted_30_14).
//
// §30.14: `sealed abstract` + all-sendable closed hierarchy → the whole
// hierarchy is sendable (coinductive evaluation over the closed set).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed abstract class Shape public abstract method Area(): int; end Shape; sealed class Circle : Shape public R: int := 0; public override method Area(): int => 0; destructor end destructor; end Circle; sealed class Rect : Shape public W: int := 0; public H: int := 0; public override method Area(): int => 0; destructor end destructor; end Rect; class C public static method Test() using var ch := new Channel<Shape>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
