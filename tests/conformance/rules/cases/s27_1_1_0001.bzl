//! case: s27_1_1_0001
//! doc: spec
//! section: 27.1.1
//! title: a wider-parameter function value is usable where a narrower one is expected
//! rule: Parameter slots of class or interface type are contravariant — the
//!   target's parameter type must implicitly reference-convert to the value's;
//!   a `(Animal) -> ()` value is usable where `(Dog) -> ()` is expected.
//! cites: 27.1.1, 5.1
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
  public method Demo()
    var handleAnimal: (Animal) -> () := lambda(a: Animal) => Console.WriteLine("handled");
    var handleDog: (Dog) -> () := handleAnimal;
    handleDog(new Dog());
  end Demo;
end Program;
