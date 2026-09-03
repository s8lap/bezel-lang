// §28.3 *Borrow lifetime rules* clause 1 (v0.286), issue #3744 — the ACCEPT
// boundary of the FIELD-channel widening on the SOURCE-KIND axis.
//
// §28.3 clause 1: "A borrow whose source *does* outlive the object **is**
// admissible: the idiomatic form binds it through a constructor parameter into
// a non-owned class-typed field (the `_owner: Customer` back-reference of
// §28.5), where the caller-side source outlives the constructed object."
//
// `p` here is a PLAIN (borrow) parameter, not an `owned` one, so this method
// disposes nothing and the caller's source outlives the call. The widened retry
// must not reject it: it asks `symbolIsOwnedObligation` — the predicate fix
// #3746 introduced for exactly this line, `owned` and not `out owned` — rather
// than treating every parameter as a frame-bounded source.
//
// RUN: %bezelc -c %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class Box
  public back: R?;
  public method Bind(p: R): int
    var mk := lambda(): int => do
      this.back := p;
      return 0;
    end;
    return mk();
  end Bind;
end Box;
class C
  public static method Main(): int
    using var r := new R();
    using var b := new Box();
    Console.WriteLine((b.Bind(r) + b.back!.V()).ToString());
    return 0;
  end Main;
end C;
