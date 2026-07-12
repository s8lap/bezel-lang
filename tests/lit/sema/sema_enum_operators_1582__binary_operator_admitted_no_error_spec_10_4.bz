// Ported from tests/sema/enum_operators_1582_test.cpp
//   (BEZEL_TEST sema_enum_operators_1582, binary_operator_admitted_no_error_spec_10_4).
//
// §10.4/A.3 (v0.183): a well-formed binary `operator +` inside an enum body
// is admitted — no "may not declare" (§10.5) diagnostic. Before #1582 this
// is the case the issue calls out as the sema member filter's gap.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Priority Low, Medium, High; public static method operator +(a: Priority, b: int): Priority => Priority(int(a) + b); end Priority; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
