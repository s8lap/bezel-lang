//! case: s35_11_0002
//! doc: spec
//! section: 35.11
//! title: choices are invariant in their type parameters
//! rule: No variance. Variance attaches to interface and function-type parameters
//!   only; choices, like records and classes, are invariant. A Result<Derived, E>
//!   is not a Result<Base, E>; the conversion is an explicit re-wrap by match.
//! cites: 35.11, 14.3
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
  Filled(value: T),     // unmarked payload: borrow-bearing at a class T, and copyable
  Empty;
end Box;

public class Program
  public static method Main()
    using var dog := new Dog();
    var d := Box<Dog>.Filled(dog);   // a named source, not a temporary (§35.3)
    var a: Box<Animal> := d;         // ERROR: Box<Dog> is not a Box<Animal>
  end Main;
end Program;
