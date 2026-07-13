// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, const_member_string_ok_1564_22_12).
//
// A `string`-typed enum const is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Kind A, B; public const Label: string := "k"; end Kind;

// NODIAG-NOT: error
// NODIAG-NOT: §
