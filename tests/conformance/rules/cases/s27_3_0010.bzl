//! case: s27_3_0010
//! doc: spec
//! section: 27.3
//! title: a lambda's written wider parameter type is admitted at a narrower target slot
//! rule: an **explicitly written** parameter type is judged as a value's
//!   parameter slot under §27.1.1: the target's parameter type must be
//!   identical to it or, at a class or interface slot, implicitly
//!   reference-convert to it, so `var f: (Dog) -> () := lambda(a: Animal) =>
//!   a.Speak();` is admitted and `lambda(d: Dog)` at an `(Animal) -> ()`
//!   target is rejected
//! cites: 27.3, 27.1.1
//! expect: run
//! stdout: woof
module Conf;

public class Animal
  public constructor()
  end constructor;

  public method Speak()
    Console.WriteLine("woof");
  end Speak;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Program
  public static method Main()
    var f: (Dog) -> () := lambda(a: Animal) => a.Speak();
    using var d := new Dog();
    f(d);
  end Main;
end Program;
