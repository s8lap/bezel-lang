//! case: s11_5_0007
//! doc: spec
//! section: 11.5
//! title: virtual and override together are rejected as mutually exclusive
//! rule: abstract, virtual, override, and sealed override are mutually
//!   exclusive -- at most one may appear.
//! cites: 11.5
//! expect: reject
//! diagnostic: mutually exclusive
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(): string => "generic sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public virtual override method Speak(): string => "Woof";
end Dog;
