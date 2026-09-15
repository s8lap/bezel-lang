//! case: s11_5_0005
//! doc: spec
//! section: 11.5
//! title: the canonical modifier order is admitted on an override
//! rule: visibility static (async|pure) (abstract|virtual|override|sealed
//!   override) (owned|scoped) readonly member-kind Name ...
//! cites: 11.5
//! expect: accept
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(): string => "generic sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public override method Speak(): string => "Woof";
end Dog;
