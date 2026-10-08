// RUN: %bezelc -c -o %t.o %s
//
// Spec §17.15 / §22.16, v0.216 (impl #2098) — ACCEPT forms compile cleanly:
// the atom in expression, argument, statement, and contract-clause position;
// the ascribed clause; the nullable-`null` clause (§21.1's dead-null binding
// — permanently null, legal); the marked-transfer clause in ORDINARY
// (non-clause) position; nested atoms; shadowing per the §21.1 nested-scope
// rule.
module M;
class Res
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D" + Id.ToString()); end destructor;
end Res;
class W
  public static method Sink(owned r: Res): int
    var v := r.Id;
    r.Dispose();
    return v;
  end Sink;
end W;
class C
  public static method F(x: int): int
    return x * 2;
  end F;

  // #2098 review F2 control: an expression-bodied member whose atom clause
  // is consumed UNCONDITIONALLY stays accepted (the `=>` staged rejections
  // fire only on the mismatch / conditional / closure shapes).
  // §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.
  public static method Take(): int => (var c := new Res(8); W.Sink(move c));

  // §22.16: the motivating position — a clause names a subresult.
  private _capacity: int := 100;
  public method Bounded(n: int): int
    requires((var m := n + n; m >= n and m <= _capacity))
    return n + n;
  end Bounded;

  public static method Main(): int
    // Expression / operand / argument positions; nested atoms.
    var area := (var w := 6; w * w);
    var v := C.F((var n := 3; n + 1)) + (var a := (var b := 2; b + b); a);
    // Ascription with §5.1 typed-context widening.
    var lg := (var n: long := 40; n + 2L);
    // §21.1 dead-null binding: legal with the nullable ascription.
    var z := (var s: Res? := null; (if s = null then 1 else 0));
    // Marked-transfer clause in ordinary position (K3.4), consumed by a
    // move argument inside the atom (§A.4 / proposal 0090, v0.246).
    var r := new Res(9);
    var got := (var t := owned r; W.Sink(move t));
    // Shadowing an outer local follows the §21.1 nested-scope rule.
    var n := 5;
    var sh := (var n := 100; n + 1);
    // Statement position under §21.9's discard regime.
    _ := (var q := 2; C.F(q));
    return area + v + int(lg) + z + got + sh + n;
  end Main;
end C;
