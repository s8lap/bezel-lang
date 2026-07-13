// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, same_enum_comparison_accepted_1137).
//
// Same-enum comparison stays accepted (#1137 must not over-reject).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green; end Color; class C public static method Main(): int var c: Color := Color.Green; if c = Color.Green then return 1; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
