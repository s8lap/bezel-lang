// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/linq_key_hashable_tkey_5959_test.cpp
//   (BEZEL_TEST sema_linq_key_hashable_tkey_5959,
//    todictionary_function_key_rejected_stdlib_5_member_where_clause).
//
// §14.2 + §3.6 hashable row clause (e) (v0.338): stdlib §5 declares
// `ToDictionary<TKey>(key: (T) -> TKey): owned Dictionary<TKey, T> where
// TKey: hashable` (stdlib §5) — "an ordinary
// member-level `where` clause … the ordinary constraint form of language
// spec §14.2 … on the member's own type parameter". A function type is not
// hashable (§3.6 clause (e): a lambda equals only itself), so the call is a
// compile error at the instantiation site. Pre-fix (fix #5959) the
// ToDictionary typing arms consulted no gate at all: the program compiled
// and ran, and the wrong key landed in the Dictionary.
//
// The CHECK lines pin §14.2's three required nouns — the constraint, the
// ARGUMENT (`(int) -> int`, which the diagnostic had to gain a function-type
// speller to print), and the parameter it turns on (`TKey`) — plus §3.6
// clause (e)'s structural reason (§3.6 rule 4 "named in diagnostics").

module M; static class Demo public static method Twice(n: int): int => n * 2; public static method Run() var a := new Array<int>(2); a.Add(1); using var d := a.ToDictionary(lambda(x: int): (int) -> int => Demo.Twice); end Run; end Demo; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: ToDictionary()'s `TKey` type argument `(int) -> int` does not satisfy the constraint `where TKey: hashable` (§14.2; stdlib §5)
// CHECK-SAME: : it is a function type, which is not **hashable** (§3.6 clause (e)