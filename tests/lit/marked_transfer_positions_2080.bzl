// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §28.3 K3.4 / axiom A7 (narrowed) / K5.1 / K8.3 / §35.5 — v0.209
// (impl #2080): the arm-position transfer marker's REJECT surface.
// "Admission is positional, enforced semantically: the entire RHS of a
// binding or assignment, an if-expression branch, or an expression-`case`/
// `when` arm result. Everywhere else — `??` operands, operator operands,
// guards, argument subexpressions — the atom is a compile error naming A7's
// narrowed bound." Plus: the K8.3 scrutinee-arm freeze; the K7.4
// maybe-consumed read-after reject; the first-landing operand bound (plain
// owned class/interface LOCALS; `owned` parameters tracked with the
// residuals); and the §35.5 owning-match scrutinee marker's targeted
// rejection (#2124).
//
// NOTE on CHECK order: sema's diagnostics (the A7 positional rejects and the
// §35.5 scrutinee routing — methods A/B/E/I) are all emitted before the
// ownership pass's (methods F/G/H); the methods are ordered so the emission
// order matches the file order.
module M;
class Res
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D"); end destructor;
end Res;
class Cell
  public V: int;
  public constructor(v: int) V := v; end constructor;
end Cell;
choice Opt None, Some(owned value: Cell); end Opt;
class C
  public static method T(): bool
    return true;
  end T;

  // (1) A7: a `??` operand is not an admission position.
  public static method A(): int
    var a := new Res(1);
    var q := a ?? owned a;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the transfer marker `move a` is admitted only as
    // CHECK-SAME: (axiom A7, narrowed v0.209; §28.3 / A.7 MarkedTransfer)
    return 0;
  end A;

  // (2) A7: a return operand is not an admission position (the unmarked
  // `return a;` under an `owned` return IS the transfer, K3.2/K3.13).
  public static method B(): owned Res
    var a := new Res(1);
    return owned a;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the transfer marker `move a` is admitted only as
  end B;

  // (3) A7: a guard is not an admission position.
  public static method E(n: int): int
    var b := new Res(1);
    var v := (case n of _ when owned b <> null => 1, _ => 0);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the transfer marker `move b` is admitted only as
    b.Dispose();
    return v;
  end E;

  // (4) §35.5 (F4): the owning-match scrutinee marker parses in the atom and
  // draws its own targeted rejection until the consuming lowering lands.
  public static method I(o: Opt): int
    var r := (case owned o of None => 0, _ => 1);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the owning-match consuming scrutinee (`case move o of {{.*}}`) is not implemented in this version
    // CHECK-SAME: (§35.5, v0.200; #2124)
    return r;
  end I;

  // (5) K8.3 / K5.1: a `case` arm result may not transfer the scrutinee's
  // own binding — the scrutinee borrow is live through the arm's completion.
  public static method F(): int
    var s := new Res(1);
    var r := (case s of _ => owned s);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: a `case` arm result may not transfer the scrutinee's own binding (`owned s`)
    // CHECK-SAME: (K8.3 / §28.3 K5.1, v0.209)
    return r.Id;
  end F;

  // (6) K7.4: a marked binding is maybe-consumed after the construct — a
  // read there is rejected.
  public static method G(): int
    var a := new Res(1);
    var x := (if C.T() then owned a else new Res(2));
    var bad := a.Id;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: local `a` may have already been consumed on another branch
    x.Dispose();
    return bad;
  end G;

  // (7) First-landing operand bound: an `owned` PARAMETER in arm position is
  // declined with reasons (per-path scope-end machinery for parameters is
  // tracked with #2080's residuals).
  public static method H(owned p: Res): int
    var v := (if C.T() then owned p else new Res(2)).Id;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the arm transfer marker `owned p` is supported only on a plain `var` owned local
    // CHECK-SAME: (§28.3 K3.4/K7.4, v0.209; #2080)
    return v;
  end H;

  // (8) §28.10 (review note): a `using var` binding is scope-pinned — the
  // arm transfer marker draws the same rejection the `move` form does.
  public static method K(): int
    using var u := new Res(1);
    var x := (if C.T() then owned u else new Res(2));
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot move out of `using var` `u`
    // CHECK-SAME: scope-bound (§28.3)
    x.Dispose();
    return 0;
  end K;

  // (9) K7.4 loop back-edge: a marked binding consumed inside a loop body is
  // maybe-consumed around the back edge — the second iteration's consume
  // (and any read) is rejected ("no flow around a back edge into a use").
  public static method J(n: int): int
    var a := new Res(1);
    var acc := 0;
    var i := 0;
    while i < n do
      var x := (if i = 0 then owned a else new Res(2));
      // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot consume `a`: it may have already been consumed on another branch
      acc := acc + x.Id;
      x.Dispose();
      i := i + 1;
    end;
    return acc;
  end J;
end C;
