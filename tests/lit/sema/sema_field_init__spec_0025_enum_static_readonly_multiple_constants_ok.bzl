// Ported from tests/sema/field_init_restrictions_test.cpp
//   (BEZEL_TEST sema_field_init, spec_0025_enum_static_readonly_multiple_constants_ok).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green, Blue; public static readonly First: int := Red; public static readonly Second: int := Green; end Color;

// NODIAG-NOT: error
// NODIAG-NOT: §
