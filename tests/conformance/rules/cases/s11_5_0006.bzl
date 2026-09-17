//! case: s11_5_0006
//! doc: spec
//! section: 11.5
//! title: visibility after override is rejected
//! rule: visibility static (async|pure) (abstract|virtual|override|sealed
//!   override) (owned|scoped) readonly member-kind Name ... The compiler
//!   rejects out-of-order modifiers.
//! cites: 11.5
//! expect: reject
//! diagnostic: order
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(): string => "generic sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  override public method Speak(): string => "Woof";
end Dog;
