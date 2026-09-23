// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/string_ordering_nullable_1575_test.cpp
//   (BEZEL_TEST string_ordering_nullable_1575, mixed_nullable_operand_rejected).
//
// §4.6 (#1575): a mixed nullable/non-nullable pair (`string?` vs `string`) is
// rejected too — only ONE operand needs to be nullable to require narrowing.

module M; class C public static method Main(): int var a: string? := null; var b: string := "x"; if a < b then return 1; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `<` requires non-nullable operands (`string`)
// CHECK-SAME: (§4.6 / §7.11)
