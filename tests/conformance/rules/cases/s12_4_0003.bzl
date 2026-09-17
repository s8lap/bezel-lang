//! case: s12_4_0003
//! doc: spec
//! section: 12.4
//! title: virtual on a member of a sealed class is rejected
//! rule: A member of a `sealed` class may not be declared `virtual`: the class
//!   admits no subclass, so a new virtual slot on it can never be filled, and
//!   an inert modifier that reads as load-bearing misleads every reader of the
//!   declaration.
//! cites: 12.4, 35.2, 9.2
//! expect: reject
//! diagnostic: virtual
//! issue: 4391
module Conf;

public sealed class Bulldog
  public constructor()
  end constructor;

  public virtual method Speak(): string => "Woof!";
end Bulldog;
