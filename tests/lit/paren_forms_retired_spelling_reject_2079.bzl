// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// migrate_paren_exprs: skip-file  (this file holds the RETIRED pre-v0.208
// spellings on purpose — the migration script must never rewrite it)
//
// Spec §20 / §19.1 / §19.7 / §9.5, v0.208 (proposal 0057; impl #2079) — HARD
// CUTOVER: the postfix conditional `x if c else y`, the `end`-closed
// expression-position `case`/`when`, and the `with … end` record update all
// leave the grammar; each becomes a parse error whose diagnostic names the
// paren-delimited replacement. One retired site yields ONE diagnostic; the
// four sites below produce exactly four errors, in source order.
module M;
record Point(X: int, Y: int);
class C
  public static method A(): int
    var x := 1 if true else 2;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the postfix conditional `x if c else y` is the retired pre-v0.208 spelling
    // CHECK-SAME: `(if c then x else y)`
    // CHECK-SAME: (§20, v0.208)
    return 0;
  end A;

  public static method B(): int
    var g := case 1 of 1 => 10, _ => 20 end;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: an expression-position `case
    // CHECK-SAME: wrap it and drop `end`
    // CHECK-SAME: (§19.1/§19.7, v0.208)
    return 0;
  end B;

  public static method D(): int
    var w := when true => 1, _ => 2 end;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: a `when`-led expression is retired (§19 / §22.4, v0.289)
    // CHECK-SAME: use the parenthesized conditional expression
    // CHECK-SAME: (`(if c1 then a elif c2 then b else z)`)
    return 0;
  end D;

  public static method E(): int
    var p := new Point(1, 2);
    var q := p with X := 10 end;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `with Name := value,
    // CHECK-SAME: keyword-attached parentheses and no `end`
    // CHECK-SAME: (§9.5, v0.208)
    return 0;
  end E;
end C;
