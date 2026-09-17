//! case: s22_7_0003
//! doc: spec
//! section: 22.7
//! title: a this-chain argument may not read a field of the not-yet-constructed instance
//! rule: A `this(...)` argument may read neither a primary-constructor
//!   parameter nor any field of `this` — in the chaining position no field
//!   of the declaring type is yet assigned (§22.8's step-1-before-step-2
//!   sequence, restated at §22.7's chaining-position clause).
//! cites: 22.8, 12.8, 7.9
//! expect: reject
//! diagnostic: chaining position
module Conf;

public class Label
  private _text: string;
  private _n: int;

  public constructor(text: string, n: int)
    _text := text;
    _n := n;
  end constructor;

  public constructor()
    this(_text, 0);
  end constructor;
end Label;
