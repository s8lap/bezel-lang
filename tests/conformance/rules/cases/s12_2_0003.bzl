//! case: s12_2_0003
//! doc: spec
//! section: 12.2
//! title: an invisible private base member does not block the name in a derived class
//! rule: A private base member never blocks -- two same-named private
//!   fields in base and derived are distinct members and legal... When a
//!   base member was invisible at the derived declaration site, the derived
//!   declaration creates a distinct member.
//! cites: 12.2
//! expect: accept
module Conf;

public class Animal
  private Speak: int := 0;

  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public method Speak(): string => "Woof";
end Dog;
