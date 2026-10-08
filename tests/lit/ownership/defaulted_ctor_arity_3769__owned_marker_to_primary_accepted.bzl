// RUN: %bezelc -c -o %t.o %s
//
// Issue #3769: the ACCEPT half of the `new`-site repro. `new Base(move x)`
// with the §22.5 default omitted is the CORRECT spelling of the transfer, and
// on the parent it had no correct spelling at all: the marker was rejected with
// "this parameter position does not take ownership", naming a position that is
// in fact `owned`. Compiling clean is the whole assertion.
// per §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.

module M;

class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;

class Base(owned r: Res, tag: int := 5)
  private owned _s: Res := r;
  private _t: int := tag;
  public method Id(): int
    return _t + _s.Id();
  end Id;
end Base;

class C
  public static method Main(): int
    var x := new Res(6);
    using var b := new Base(move x);
    return b.Id();
  end Main;
end C;
