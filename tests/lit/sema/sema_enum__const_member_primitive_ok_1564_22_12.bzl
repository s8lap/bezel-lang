// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, const_member_primitive_ok_1564_22_12).
//
// A primitive-typed enum const is accepted, exactly as on a class.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Kind A, B; public const Limit: int := 42; end Kind;

// NODIAG-NOT: error
// NODIAG-NOT: §
