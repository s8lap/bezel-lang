//! case: s12_2_0001
//! doc: spec
//! section: 12.2
//! title: a derived class may not redeclare a visible inherited member's name
//! rule: Declaring a member in a derived class with the same name as any
//!   inherited member visible at the derived class's declaration site is a
//!   compile error, unless the declaration is an override of a virtual
//!   member.
//! cites: 12.2
//! expect: reject
//! diagnostic: hiding
module Conf;

public class Animal
  public constructor()
  end constructor;

  public method Speak(): string => "generic sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public method Speak(): string => "Woof";
end Dog;
