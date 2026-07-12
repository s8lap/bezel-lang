// Ported from tests/sema/generic_inconsistent_type_arg_896_test.cpp
//   (BEZEL_TEST generic_inconsistent_type_arg_896, consistent_int_int_accepted).
//
// §14.1 / #896: Merge<T>(int?, int?) — both args agree on T=int. ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class P public static method Merge<T>(a: T?, b: T?): T where T: value return a ?? b ?? default(T); end Merge; public static method Main(): int var x: int? := 1; var z: int? := 2; return Merge(x, z); end Main; end P;

// NODIAG-NOT: error
// NODIAG-NOT: §
