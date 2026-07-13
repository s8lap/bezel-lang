// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, mutually_recursive_owned_sendable_classes_accepted_30_14).
//
// §30.14: Mutually-recursive owned sendable classes — coinductive rule admits
// them. A <-> B where both are sealed with only owned fields of each other.
// Per spec: "Mutually-recursive owned references among sendable classes are
// admitted; the property is evaluated coinductively."
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class NodeA public Val: int := 0; public owned Next: NodeB?; destructor end destructor; end NodeA; sealed class NodeB public Val: int := 0; public owned Prev: NodeA?; destructor end destructor; end NodeB; class C public static method Test() using var chA := new Channel<NodeA>(); using var chB := new Channel<NodeB>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
