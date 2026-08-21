// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §5.1, spec v0.220): `int += int?` — §7.11's mixed
// lifting types `int + int?` as `int?`, and a nullable value never implicitly
// converts to the non-nullable `int` the store-back requires (§5.1's nullable
// conversions run T → T? only). "In every other case the compound form is a
// compile error" (§6.6). Before the fix the nullable-RHS gate skipped the
// check and codegen fed the `{i8, i32}` wrapper struct to the integer
// overflow intrinsic — a malformed-IR compiler abort.

module M; class C public static method Main(): int var i: int := 5; var n: int? := 3; i += n; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not defined when the right-hand operand is nullable (`int?`) — the arithmetic, bitwise, or shift operators do not lift over a nullable value type (§7.11, v0.267); narrow first (§7.4 check, `?? <fallback>` (§7.7), or `!` (§7.8)) and use the non-nullable compound form (e.g. `b := (b ?? 0) + 1;`) (§6.6)
