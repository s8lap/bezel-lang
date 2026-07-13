// Ported from tests/sema/send_600_channel_rules_test.cpp
//   (BEZEL_TEST send_600_channel_rules, array_int_as_channel_element_accepted_30_14).
//
// §30.14 stdlib table: Array<int> — primitives are sendable values;
// a borrowing collection of sendable-value elements is sendable.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Test() using var ch := new Channel<Array<int>>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
