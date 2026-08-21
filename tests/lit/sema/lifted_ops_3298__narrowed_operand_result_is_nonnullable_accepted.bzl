// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// #3298 (§7.11 / §6.2 / §17.5, spec v0.286) — ACCEPTANCE control for the
// narrowing sentence: "The rule reads the operand's **static** type, so an
// operand that §7.4 has narrowed non-null is a non-nullable operand and the
// ordinary operator applies to it unchanged; no separate rule is needed for
// that case" (§7.11).
//
// Every line below stores the operator's RESULT into a NON-NULLABLE target,
// so the file observes the result TYPE and not merely "no rejection fired":
//   * `n + 1`  — mixed narrowed-`int?`/`int`, result `int`
//   * `n + w`  — §6.2 promotion still runs on the normalized operands, `long`
//   * `-n`     — the unary family takes the same reading
//   * `n in 1..10` — §17.5's range gate admits the narrowed operand, and the
//                    backend unwraps its `{i8, T}` box for the comparisons
//
// The first draft of the #3298 rejection typed all four as nullable and this
// whole file was a false reject.

module M;
class C
  public static method Main(): int
    var n: int? := 5;
    var w: long := 7;
    var m: int := 0;
    var acc: long := 0;
    var neg: int := 0;
    var b: bool := false;
    if n <> null then
      m := n + 1;
      acc := n + w;
      neg := -n;
      b := n in 1..10;
    end if;
    return m + int(acc) + neg + (if b then 1 else 0);
  end Main;
end C;

// NODIAG-NOT: error
