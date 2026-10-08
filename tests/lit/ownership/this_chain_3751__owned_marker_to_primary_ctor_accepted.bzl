// RUN: not %bezelc -c -o %t.o %s 2>&1 | FileCheck --check-prefix=DIAG --allow-empty %s
//
// Issue #3751: the ACCEPT half. `this(owned x)` against a §22.10 primary
// constructor whose parameter is `owned` was REJECTED before the fix, with a
// diagnostic naming a position that IS `owned` ("this parameter position does
// not take ownership") — so no correct spelling of the transfer existed. It is
// admitted here, and the transfer marks `x` moved-from, which is what removes
// the §28.3 auto-drop and its use-after-free.
//
// FLIPPED for fix #5132 (§11.1 / §22.6, v0.308): the §11.1 "same parameter
// list" sense is now §22.6 overload identity (arity, parameter types, ordinary
// `owned` on a participating position), so the body constructor `(owned x: Res)`
// and the primary `(owned r: Res)` — same type and same modifier, differing
// only by name — are a clash. This file is now a REJECT control that names
// the §11.1 redeclares-primary diagnostic.
// per §A.4 / proposal 0090 (v0.246): call-site transfer uses `move`, not `owned`.

// DIAG: body constructor redeclares the primary constructor
// DIAG-SAME: (§11.1, v0.308)

module M;
class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;
class Base(owned r: Res)
  private owned _s: Res := r;
  public constructor(owned x: Res)
    this(move x);
  end constructor;
  public method Id(): int
    return _s.Id();
  end Id;
end Base;
