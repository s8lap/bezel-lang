// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, sealed_class_primitive_string_fields_accepted_30_14).
//
// §30.14 Rule 1+2: sealed class with only primitive and string fields → sendable.
// Channel<Report> with such a class must be accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; sealed class Report public Id: int := 0; public Name: string := ""; destructor end destructor; end Report; class C public static method Test() using var ch := new Channel<Report>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
