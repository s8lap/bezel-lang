//! case: s27_1_1_0002
//! doc: spec
//! section: 27.1.1
//! title: a narrower-parameter function value is not usable where a wider one is expected
//! rule: Parameter slots of class or interface type are contravariant — the
//!   target's parameter type must implicitly reference-convert to the value's;
//!   a `(Dog) -> ()` value is not usable where `(Animal) -> ()` is expected.
//! cites: 27.1.1, 5.1
//! expect: reject
//! diagnostic: contravariant
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
  public method Demo()
    var handleDog: (Dog) -> () := lambda(d: Dog) => Console.WriteLine("handled");
    var handleAnimal: (Animal) -> () := handleDog;
  end Demo;
end Program;
