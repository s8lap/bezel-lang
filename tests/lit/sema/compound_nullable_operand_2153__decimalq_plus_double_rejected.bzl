// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §6.2 / §7.11, spec v0.220): `decimal? += 5.0` — decimal and
// double have no implicit conversion in either direction (§6.2's own example;
// §6.6 names `d += 5.0` a compile error for the non-nullable form), and §7.11
// lifting changes nothing about the underlying admission. The non-nullable
// `decimal += 5.0` cell is already rejected (#1689); the nullable-LHS gate
// skipped this one — the #2141 lowering then fed the raw double to the
// decimal intrinsic (garbage bits).

module M; class C public static method Main(): int var d: decimal? := 1.0m; d += 5.0; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not defined when the target is nullable (`decimal?`) — the arithmetic, bitwise, or shift operators do not lift over a nullable value type (§7.11, v0.267); narrow first (§7.4 check, `?? <fallback>` (§7.7), or `!` (§7.8)) and use the non-nullable compound form (e.g. `b := (b ?? 0) + 1;`) (§6.6)
