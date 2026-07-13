// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, partial_explicit_list_bare_identifier_rejected).
//
// §14.5 / #1687: the same partial-list rule applies to a bare-identifier
// (no-dot, same-class) generic call, not just the dotted member-access form.

module M; class C public static method Pick<TFirst, TSecond>(a: TFirst, b: TSecond): TFirst return a; end Pick; public static method Main(): int var x := Pick<int>(1, "s"); return x; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `Pick` supplies 1 explicit type argument(s), but `Pick` declares 2 type parameter(s) — an explicit type-argument list must supply exactly as many arguments as the method declares type parameters; there are no partial lists
// CHECK-SAME: (§14.5)
