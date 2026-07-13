// Ported from tests/sema/enum_duplicate_values_1577_test.cpp
//   (BEZEL_TEST sema_enum_dup_values, declaration_level_aliasing_is_legal_spec_10_2).
//
// `Legacy := 1, Current := 1` at the declaration itself draws no diagnostic;
// only a later `case` naming both is restricted (tested below).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E A := 1, B := 1; end E; class C public method M(): int return int(E.A); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
