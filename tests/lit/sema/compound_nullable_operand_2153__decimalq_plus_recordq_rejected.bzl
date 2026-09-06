// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §6.2 / §25.2 / §7.11, spec v0.220): the exact issue cell —
// `decimal? += record?`. No built-in operator exists between a primitive and
// a record (§6.2's promotion covers primitive numeric operands only), the
// record declares no `operator +`, and user-defined operators do not lift
// over nullable operands anyway (§7.11 — deferred, #1132). Before the fix
// sema admitted the statement and the #2141/#2149 lowering reinterpreted the
// record payload's bits as a decimal: `decimal?(1.5m) += P?(7, 9)` silently
// stored the garbage value 27792441192883578369065615369 (probe, @dd33abd).
//
// fix #3830 (v0.288): both `d` and `r` are PARAMETERS — a literal-
// initialized `decimal?` local narrows (its initializer is a decimal
// literal), and a `new P(...)`-initialized `P?` local narrows too (a
// non-nullable-typed initializer proves non-null, §7.4), so as LOCALS both
// operands would be narrowed and the message this file pins would change:
// with `r` narrowed the tail reads "`P` declares no applicable `operator +`
// overload" instead of "user-defined operators do not lift over a nullable
// operand" — a real, and here undesired, diagnostic-family change (the
// point of THIS row is the nullable-operand tail, not the missing-overload
// one). A parameter is never narrowed by what the caller passes.

module M;
record P(X: int, Y: int)
end P;
class C
  public static method Chk(d: decimal?, r: P?): int
    d += r;
    return 0;
  end Chk;
  public static method Main(): int
    return Chk(1.5m, new P(7, 9));
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: no built-in operator `+` is defined between `decimal?` and record type `P?` (§6.2's promotion covers primitive numeric operands only), and user-defined operators do not lift over a nullable operand (§7.11 — deferred)
