// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, string_as_channel_element_accepted_30_14).
//
// §30.14: string is sendable (crosses as shared immutable reference).
// Channel<string> must be accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Test() using var ch := new Channel<string>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
