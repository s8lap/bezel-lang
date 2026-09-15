//! case: s35_11_0002
//! doc: spec
//! section: 35.11
//! title: choices are invariant in their type parameters
//! rule: No variance. Choices, like records and classes, are invariant. A
//!   Result<Derived, E> is not a Result<Base, E>; the conversion is an explicit
//!   re-wrap by match.
//! expect: reject
//! diagnostic: invariant
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public choice Box<T>
  Filled(owned value: T),
  Empty;
end Box;

public class Program
  public static method Main()
    var d := Box<Dog>.Filled(new Dog());
    var a: Box<Animal> := d;   // ERROR: Box<Dog> is not a Box<Animal> — choices are invariant
  end Main;
end Program;
