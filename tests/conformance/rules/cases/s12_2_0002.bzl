//! case: s12_2_0002
//! doc: spec
//! section: 12.2
//! title: overriding a virtual base member is the sanctioned way to reuse its name
//! rule: Declaring a member in a derived class with the same name as any
//!   inherited member visible at the derived class's declaration site is a
//!   compile error, unless the declaration is an override of a virtual
//!   member with a matching signature.
//! cites: 12.2, 12.1
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
