// RUN: %bezelc -c -o %t.o %s
//
// Issue #3769, ANTI-OVER-REJECT — the row that pins `CtorSelect::ChainSite`.
//
// The two LOWERING resolvers this fix must agree with do not agree with each
// other. A `new T(args)` site lowers through codegen's `ctorForArgs`, which
// tries an explicit constructor whose formals the arguments can FILL with §22.5
// defaults admitted (fix #3082). A `this(...)` chain lowers through
// `emitChainCall`, which uses `ctorByArity` — EXACT arity, no defaults — and
// then falls through to the §22.10 primary arm; this class has no primary, so
// the chain below lowers to NOTHING (measured: the plain twin of this program
// prints `0/0`).
//
// Ownership must therefore NOT classify `x` as transferred here. Declaring a
// transfer into a constructor that is never called would demand
// `this(owned x)`, suppress the §28.3 auto-drop, and LEAK the object — a
// soundness widening in the opposite direction from the hole being closed. The
// auto-drop warning that fires instead is the correct diagnostic: it names the
// free that keeps this program leak-free.

module M;

class Res
  private _id: int;
  public constructor(id: int) _id := id; end constructor;
  public method Id(): int => _id;
end Res;

class B
  private owned _f: Res;
  private _n: int;
  public constructor(owned a: Res, b: int := 3)
    _f := move a;
    _n := b;
  end constructor;
  public constructor(owned x: Res, y: bool)
    this(x);
  end constructor;
  public method Id(): int
    return _n;
  end Id;
end B;
