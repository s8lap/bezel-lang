// §28.3 *Borrow lifetime rules*, clause 1 — the §28.5 back-reference idiom
// (v0.286); the ACCEPT control for issue #3746.
//
// §28.3: "A borrow whose source *does* outlive the object **is** admissible:
// the idiomatic form binds it through a constructor parameter into a
// non-owned class-typed field (the `_owner: Customer` back-reference of
// §28.5), where the caller-side source outlives the constructed object."
//
// This is the program that admission is written for: `p` is a PLAIN (borrow)
// parameter, so the caller keeps the value alive. #3746 widened the store
// check to `owned` parameters ONLY — `symbolIsOwnedObligation` answers false
// for an unmarked parameter — so this row must be accepted exactly as before,
// and must still print `5`.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class Box
  public r: R?;
  public method Take(p: R)
    this.r := p;
  end Take;
end Box;
class C
  public static method Main(): int
    using var q := new R();
    using var b := new Box();
    b.Take(q);
    Console.WriteLine(b.r!.V().ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
