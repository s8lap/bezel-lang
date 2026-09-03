// RUN: %bezelc -c -o %t.o %s
//
// Issue #3751: the ACCEPT half. `this(owned x)` against a §22.10 primary
// constructor whose parameter is `owned` was REJECTED before the fix, with a
// diagnostic naming a position that IS `owned` ("this parameter position does
// not take ownership") — so no correct spelling of the transfer existed. It is
// admitted here, and the transfer marks `x` moved-from, which is what removes
// the §28.3 auto-drop and its use-after-free.

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class Base(owned r: Res)
  private owned _s: Res := r;
  public constructor(owned x: Res)
    this(owned x);
  end constructor;
  public method Id(): int
    return _s.Id();
  end Id;
end Base;
