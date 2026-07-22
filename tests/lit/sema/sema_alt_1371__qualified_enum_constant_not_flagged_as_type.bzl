// Ported from tests/sema/pattern_alternatives_1371_test.cpp
//   (BEZEL_TEST sema_alt_1371, qualified_enum_constant_not_flagged_as_type).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color Red, Green, Blue; end Color; class C public static method M(c: Color): int return (case c of Color.Red | Color.Blue => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
