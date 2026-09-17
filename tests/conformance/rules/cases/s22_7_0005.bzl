//! case: s22_7_0005
//! doc: spec
//! section: 22.7
//! title: an instance method call on this in the chaining position is rejected
//! rule: "A synchronous observation of `this` — an instance method ...
//!   invocation with receiver `this` ... is admitted only at points where
//!   every field ... is definitely assigned on every reaching path. In the
//!   chaining position ... any observation of `this` there is a compile
//!   error."
//! cites: 22.7
//! expect: reject
//! diagnostic: chaining position
module Conf;

public class Base
  public constructor(n: int)
  end constructor;
end Base;

public class Derived: Base
  public constructor()
    base(this.Compute());
  end constructor;

  public method Compute(): int => 1;
end Derived;
