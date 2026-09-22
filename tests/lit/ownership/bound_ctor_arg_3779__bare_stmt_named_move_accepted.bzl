// RUN: %bezelc -c -o %t.o %s
//
// Issue #3779: the ACCEPT half. §22.5 admits the transfer marker inside a named
// argument — "a transfer-marked argument carries no modifier — the marker is
// part of the argument expression ... so `M(item := move x)` is an ordinary
// named argument whose expression happens to be a `MoveExpr`, and is legal
// (v0.260)" — so `a := move x` is the correct spelling of the reordered call.
//
// Both earlier compilers rejected it: 5fbb8ab for the defaulted-arity form and
// BOTH 89eb914 and 5fbb8ab for the exact-arity form, each with "cannot move out
// of `using var` `y`" — a diagnostic about the argument that binds to the
// BORROW formal. Compiling clean is the whole assertion.

module M;

class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;

class Two
  private owned _a: Res;
  private _bid: int;
  private _c: int;
  public constructor(owned a: Res, b: Res, c: int := 1)
    _a := a;
    _bid := b.Id();
    _c := c;
  end constructor;
end Two;

class C
  public static method Main(): int
    using var y := new Res(7);
    var x := new Res(6);
    new Two(b := y, a := move x);
    var x2 := new Res(8);
    new Two(b := y, a := move x2, c := 3);
    return 0;
  end Main;
end C;
