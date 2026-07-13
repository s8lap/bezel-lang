// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_type_args_all_or_nothing_1687_test.cpp
//   (BEZEL_TEST generic_type_args_1687, record_ctor_no_type_args_rejected).
//
// §14.5 Scope / #1687 (q19): `new Pair(1, "x")` against a generic RECORD
// with no explicit type arguments and no target-type context is a compile
// error — class-/record-level type arguments are never inferred from
// constructor arguments. This is the spec's own example.

module M; record Pair<TFirst, TSecond>(First: TFirst, Second: TSecond); class C public static method Main(): int var p := new Pair(1, "x"); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `new Pair(...)` supplies no type arguments, but `Pair` declares 2 type parameter(s) — class-, record-, and interface-level type arguments are never inferred from constructor arguments; supply the full explicit list (`new Pair<...>(...)`) or, where a target type exists, use the `new()` shorthand
// CHECK-SAME: (§14.5, §17.7)
