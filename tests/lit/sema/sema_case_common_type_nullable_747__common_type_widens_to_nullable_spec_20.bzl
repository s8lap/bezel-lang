// Ported from tests/sema/case_common_type_nullable_747_test.cpp
//   (BEZEL_TEST sema_case_common_type_nullable_747, common_type_widens_to_nullable_spec_20).
//
// §20: a non-nullable `int` arm and an `int?` arm share the common type `int?`,
// so assigning the case result to an `int?` target is well-typed (no §7 error).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var x: int? := 5; var r: int? := (case x of null => 0, var n => n); return (if r = null then 0 else r!); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
