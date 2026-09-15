//! case: s5_1_0019
//! doc: spec
//! section: 5.1
//! title: a derived class reference implicitly converts to its base class
//! rule: Reference conversions are implicit: class to base class, transitively,
//!   up to Object.
//! cites: 5.1
//! expect: accept
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Program
  public static method Convert(d: Dog)
    var a: Animal := d;
  end Convert;
end Program;
