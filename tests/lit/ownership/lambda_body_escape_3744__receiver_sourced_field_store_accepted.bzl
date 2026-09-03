// §28.3 *Borrow lifetime rules* clause 2 (v0.286), issue #3744 — the ACCEPT
// boundary of the FIELD-channel widening on the SOURCE-PROVENANCE axis.
//
// §28.3 clause 2: "The assignment-site check is structural and decidable —
// parameter-sourced and receiver-sourced borrows are admissible; scope-bound
// locals and temporaries are rejected."
//
// The stored borrow is reached through the RECEIVER's own owned field, whose
// lifetime is the receiver's, so no frame of this method bounds it and the
// widened `Dest::Field` retry must resolve no owned scope-bound source at all.
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
  public owned a: R?;
  public back: R?;
  public method Link(): int
    var mk := lambda(): int => do
      this.back := this.a;
      return 0;
    end;
    return mk();
  end Link;
end Box;
class C
  public static method Main(): int
    using var b := new Box();
    b.a := new R();
    Console.WriteLine((b.Link() + b.back!.V()).ToString());
    return 0;
  end Main;
end C;
