// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §3.5, spec v0.220): `decimal? += bool?` — no operator
// `+` between decimal and bool. Before the fix this reached the #2141 decimal
// lowering with the bool payload reinterpreted through the decimal intrinsic:
// `decimal?(2.0m) += bool?(true)` silently stored the garbage value
// 1143708905468671979825532163 (probe, main @dd33abd).
//
// fix #3830 (v0.288): `d`/`b` are PARAMETERS — a literal-initialized local
// (`var d: decimal? := 2.0m;`) is §7.4-narrowed at its initializer, and
// narrowing now reaches compound assignment, so this row would stop
// exercising a genuinely nullable operand. A parameter is never narrowed by
// what the caller passes.

module M;
class C
  public static method Chk(d: decimal?, b: bool?): int d += b; return 0; end Chk;
  public static method Main(): int return Chk(2.0m, true); end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` is not defined when both operands are nullable (target `decimal?`, right-hand operand `bool?`) — the arithmetic, bitwise, or shift operators do not lift over a nullable value type (§7.11, v0.267); narrow first (§7.4 check, `?? <fallback>` (§7.7), or `!` (§7.8)) and use the non-nullable compound form (e.g. `b := (b ?? 0) + 1;`) (§6.6)
