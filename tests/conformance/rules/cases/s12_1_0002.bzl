//! case: s12_1_0002
//! doc: spec
//! section: 12.1
//! title: an override with a mismatched parameter type is rejected
//! rule: Visibility, return type, parameter types, and parameter modifiers
//!   must match the base member exactly.
//! cites: 12.1
//! expect: reject
//! diagnostic: override
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(volume: int): string => "generic sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public override method Speak(volume: double): string => "Woof";
end Dog;
