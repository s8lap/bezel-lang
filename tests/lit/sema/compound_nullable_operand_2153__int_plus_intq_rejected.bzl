// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §5.1, spec v0.220): `int += int?` — §7.11's mixed
// lifting types `int + int?` as `int?`, and a nullable value never implicitly
// converts to the non-nullable `int` the store-back requires (§5.1's nullable
// conversions run T → T? only). "In every other case the compound form is a
// compile error" (§6.6). Before the fix the nullable-RHS gate skipped the
// check and codegen fed the `{i8, i32}` wrapper struct to the integer
// overflow intrinsic — a malformed-IR compiler abort.
//
// fix #3830 (v0.288): `n` is a PARAMETER, not a literal-initialized local.
// `var n: int? := 3;` is §7.4-narrowed at its initializer, and narrowing now
// reaches compound assignment — a narrowed `n` is a genuinely non-nullable
// `int` operand, and `int += int` is trivially admitted, so this row would
// silently stop being a rejection at all if `n` stayed a narrowed local
// (verified empirically: the narrowed form compiles and computes `8`). A
// parameter is never narrowed by what the caller passes, so this keeps
// testing the genuinely nullable, unguarded RHS the row is about.

module M;
class C
  public static method Chk(n: int?): int var i: int := 5; i += n; return 0; end Chk;
  public static method Main(): int return Chk(3); end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not defined when the right-hand operand is nullable (`int?`) — the arithmetic, bitwise, or shift operators do not lift over a nullable value type (§7.11, v0.267); narrow first (§7.4 check, `?? <fallback>` (§7.7), or `!` (§7.8)) and use the non-nullable compound form (e.g. `b := (b ?? 0) + 1;`) (§6.6)
