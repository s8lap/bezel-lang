//! case: s12_8_0003
//! doc: spec
//! section: 12.8
//! title: a chain argument may not read a primary-constructor parameter
//! rule: nor, in a class with a primary constructor, a primary-constructor
//!   parameter (§22.10; the field half is §22.7's ground: nothing of this
//!   is assigned yet).
//! cites: 12.8, 22.10
//! expect: reject
//! diagnostic: chain
module Conf;

public class Base
  private _n: int;

  public constructor(n: int)
    _n := n;
  end constructor;
end Base;

public class Derived(seed: int): Base(seed)
  public constructor(n: int, k: int)
    this(n + seed);
  end constructor;
end Derived;
