//! case: s12_1_0001
//! doc: spec
//! section: 12.1
//! title: an override matching the base member exactly is admitted
//! rule: A method, property, or indexer is non-virtual unless marked
//!   virtual. To override a virtual member in a derived class, mark the
//!   matching member override.
//! cites: 12.1
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
