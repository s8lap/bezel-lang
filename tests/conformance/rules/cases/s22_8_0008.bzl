//! case: s22_8_0008
//! doc: spec
//! section: 22.8
//! title: a constructor may not assign another instance's readonly storage
//! rule: The context is per-instance, not per-type — an instance
//!   `readonly` field is assignable only through an lvalue chain rooted at
//!   `this`, so a constructor of the declaring type may not write another
//!   value's `readonly` storage held in a parameter, local, or field.
//! cites: 22.8
//! expect: reject
//! diagnostic: rooted at this
module Conf;

public class Counter
  private readonly _start: int;

  public constructor(start: int)
    _start := start;
  end constructor;

  public constructor(other: Counter)
    other._start := 0;
  end constructor;
end Counter;
