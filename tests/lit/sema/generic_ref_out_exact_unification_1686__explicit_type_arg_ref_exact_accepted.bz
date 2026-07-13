// Ported from tests/sema/generic_ref_out_exact_unification_1686_test.cpp
//   (BEZEL_TEST generic_ref_out_exact_unification_1686, explicit_type_arg_ref_exact_accepted).
//
// §14.5 / #1686 control: same explicit-`<long>` call, but the `ref`
// argument's own static type is ALSO `long` — an exact match. Must remain
// accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class Utils public static method StoreW<T>(value: T, ref slot: T) slot := value; end StoreW; end Utils; class C public static method Main(): int var v: long := 5; var s: long := 1; Utils.StoreW<long>(v, ref s); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
