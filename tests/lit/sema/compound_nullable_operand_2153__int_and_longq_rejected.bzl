// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.11 / §6.5, spec v0.220): `int &= long?` — the bitwise
// operators lift over nullable INTEGER operands (§7.11), so `int & long?` is
// `long?`, which cannot store back into the non-nullable `int` target. The
// same nullable-RHS store-back rule as the arithmetic forms, on the
// bitwise/shift branch (which previously checked only the operand KINDS and
// only for non-nullable operands).
//
// fix #3830 (v0.288): `w` is a PARAMETER, not a literal-initialized local.
// `var w: long? := 3L;` is §7.4-narrowed at its initializer, and narrowing
// now reaches compound assignment — a narrowed `w` is a genuinely
// non-nullable `long` operand here, and `int &= long` (narrow-integer
// write-back, §6.6) IS admitted, so this row would silently stop being a
// rejection at all if `w` stayed a narrowed local (verified empirically:
// the narrowed form compiles and computes `3`). A parameter is never
// narrowed by what the caller passes, so this keeps testing the genuinely
// nullable, unguarded RHS the row is about.

module M;
class C
  public static method Chk(w: long?): int var i: int := 7; i &= w; return 0; end Chk;
  public static method Main(): int return Chk(3L); end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `&=` is not defined when the right-hand operand is nullable (`long?`) — the arithmetic, bitwise, or shift operators do not lift over a nullable value type (§7.11, v0.267); narrow first (§7.4 check, `?? <fallback>` (§7.7), or `!` (§7.8)) and use the non-nullable compound form (e.g. `b := (b ?? 0) & 1;`) (§6.6)
