// RUN: %bezelc -c -o %t.o %s
//
// Issue #3769 / #3777 — the ACCEPT half of the reversed `CtorSelect::ChainSite`
// pin. The rejected twin is
// `defaulted_ctor_arity_3769__chain_to_explicit_sibling_unmarked_rejected.bzl`.
//
// Since fix #3777 a `this(...)` chain to an explicit SIBLING across an omitted
// §22.5 default really calls that sibling (`emitChainCall` selects with
// `ctorForArgs` and fills the omitted tail), so the position is an owned sink
// and §22.3's call-site marker is both REQUIRED and SUFFICIENT: `this(owned x)`
// transfers, `x` becomes moved-from, the §28.3 auto-drop does not fire, and the
// object is disposed exactly once — by `B`'s own `_f`.
//
// This is the spelling that did not exist before #3769: the unmarked form was a
// silent auto-drop and the marked form was rejected with "this parameter
// position does not take ownership", naming a position that IS `owned`.

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
    _f := a;
    _n := b;
  end constructor;
  public constructor(owned x: Res, y: bool)
    this(owned x);
  end constructor;
  public method Id(): int
    return _n;
  end Id;
end B;
