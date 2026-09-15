//! case: s22_16_0024
//! doc: spec
//! section: 22.16
//! title: a constructor's requires clause may not observe this
//! rule: A constructor's `requires` may not observe `this` in any of
//!   §22.7's senses (no field of the declaring type is assigned at
//!   clause time) — it reads parameters and statics only.
//! cites: 22.16, 22.7
//! expect: reject
//! diagnostic: this
module Conf;

public class Widget
  private _n: int := 0;

  public pure method Positive(): bool => _n > 0;

  public constructor(n: int)
    requires(Positive())

    _n := n;
  end constructor;
end Widget;
