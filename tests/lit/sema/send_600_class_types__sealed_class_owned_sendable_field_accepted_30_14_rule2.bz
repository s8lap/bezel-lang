// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, sealed_class_owned_sendable_field_accepted_30_14_rule2).
//
// §30.14 Rule 2: sealed class with an `owned` field whose type is another
// sendable sealed class → sendable (nested ownership tree all moves together).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class Inner public Val: int := 0; destructor end destructor; end Inner; sealed class Outer public owned Child: Inner := new Inner(); destructor end destructor; end Outer; class C public static method Test() using var ch := new Channel<Outer>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
