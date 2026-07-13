// Ported from tests/sema/enum_restrictions_test.cpp
//   (BEZEL_TEST sema_enum, enum_method_this_comparison_accepted_1137).
//
// `this = E.X` inside an enum method stays accepted (`this` is the same enum).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Status Ok, Error; public method IsOk(): bool => this = Status.Ok; end Status; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
