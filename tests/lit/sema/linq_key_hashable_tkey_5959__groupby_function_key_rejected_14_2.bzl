// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/linq_key_hashable_tkey_5959_test.cpp
//   (BEZEL_TEST sema_linq_key_hashable_tkey_5959,
//    groupby_function_key_rejected_stdlib_5_member_where_clause).
//
// §14.2 + §3.6 hashable row clause (e) (v0.338): stdlib §5 declares
// `GroupBy<TKey>(key: (T) -> TKey): IEnumerable<IGrouping<TKey, T>> where
// TKey: hashable` (Bezel_Standard_Library.md:3697) — the same ordinary
// member-level form as ToDictionary's, on the member's own type parameter.
// Pre-fix the GroupBy typing arm consulted no gate, so a function-typed key
// compiled silently. GroupBy is spelled here on the concrete-collection
// receiver; the gtest file covers the lazy `IEnumerable<T>` receiver arm and
// the string `IEnumerable<char>` ToDictionary arm, which are SEPARATE
// if/else-if chains in ExprTypecheck_Calls.cpp and would each be a silent
// acceptance left ungated by this file's row alone (the #3730 finding).

module M; static class Demo public static method Twice(n: int): int => n * 2; public static method Run() var a := new Array<int>(2); a.Add(1); var n := a.GroupBy(lambda(x: int): (int) -> int => Demo.Twice).Count(); end Run; end Demo; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: GroupBy()'s `TKey` type argument `(int) -> int` does not satisfy the constraint `where TKey: hashable` (§14.2; stdlib §5)
// CHECK-SAME: : it is a function type, which is not **hashable** (§3.6 clause (e)