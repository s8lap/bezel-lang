//! case: s12_4_0004
//! doc: spec
//! section: 12.4
//! title: the same virtual member on a non-sealed class is admitted
//! rule: The rejection is keyed on the class being `sealed` — a class that
//!   admits subclasses may declare a new `virtual` slot, which is what
//!   §12.1's override-matching rule exists to fill.
//! cites: 12.4, 12.1
//! expect: accept
//! issue: 4391
module Conf;

public class Dog
  public constructor()
  end constructor;

  public virtual method Speak(): string => "Woof!";
end Dog;

public class Bulldog: Dog
  public constructor()
    base();
  end constructor;

  public override method Speak(): string => "Gruff!";
end Bulldog;
