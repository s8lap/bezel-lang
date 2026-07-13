// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, excess_explicit_list_rejected).
//
// §14.5 / #1687 (p11): a LONGER explicit list (3 of 2 declared type
// parameters) is a compile error — extras are not silently dropped.

module M; static class Utils public static method Pick<TFirst, TSecond>(a: TFirst, b: TSecond): TFirst return a; end Pick; end Utils; class C public static method Main(): int var x := Utils.Pick<int, string, bool>(1, "s"); return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `Pick` supplies 3 explicit type argument(s), but `Pick` declares 2 type parameter(s) — an explicit type-argument list must supply exactly as many arguments as the method declares type parameters; there are no partial lists
// CHECK-SAME: (§14.5)
