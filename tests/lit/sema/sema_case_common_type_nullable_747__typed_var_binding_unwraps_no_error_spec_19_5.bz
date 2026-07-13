// Ported from tests/sema/case_common_type_nullable_747_test.cpp
//   (BEZEL_TEST sema_case_common_type_nullable_747, typed_var_binding_unwraps_no_error_spec_19_5).
//
// CONTROL §19.5: a TYPED binding `var n: int` over the `int?` scrutinee matches
// only non-null and binds the UNWRAPPED `int`, so the arms share the common
// type `int`; returning it from an `int` method is well-typed (no §7 error).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var x: int? := 42; return case x of null => 0, var n: int => n end; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
