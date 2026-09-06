// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §4.6, spec v0.220): `string += int?` — a nullable
// right-hand operand can never produce the non-nullable result a store into
// the `string` target requires; the same nullable-RHS rule as the numeric
// cells. Before the fix the #857 string-compound path rendered the `{i8,i32}`
// wrapper struct's tag byte: `string("v=") += int?(3)` silently produced
// "v=1" (probe, main @dd33abd).
//
// fix #3830 (v0.288): `n` is a PARAMETER — `var n: int? := 3;` is
// §7.4-narrowed at its initializer, and narrowing now reaches compound
// assignment. A narrowed `n` is a non-nullable `int`, and `string += int`
// is STILL rejected, but by the §4.6 "requires a `string` right-hand
// operand" wording, not this file's nullable-specific one — a real
// diagnostic-family change this conversion avoids. A parameter is never
// narrowed by what the caller passes.

module M;
class C
  public static method Chk(n: int?): int var s: string := "v="; s += n; return 0; end Chk;
  public static method Main(): int return Chk(3); end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not defined when the right-hand operand is nullable (`int?`) — the arithmetic, bitwise, or shift operators do not lift over a nullable value type (§7.11, v0.267); narrow first (§7.4 check, `?? <fallback>` (§7.7), or `!` (§7.8)) and use the non-nullable compound form (e.g. `b := (b ?? 0) + 1;`) (§6.6)
