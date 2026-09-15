//! case: s20_0006
//! doc: spec
//! section: 20
//! title: branches of a subclass and its base class compute a common type of the base class
//! rule: "**The common type:** the common type of branch types A and B is
//!   whichever of the two the other implicitly converts into ... a subtype into
//!   its base class or implemented interface (a §5.1 reference conversion)."
//! cites: 20, 5.1
//! expect: run
//! stdout: Woof
module Conf;

public class Animal
  public constructor()
  end constructor;

  public method Speak(): string => "sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public override method Speak(): string => "Woof";
end Dog;

public class Program
  public static method Main()
    var preferDog := true;
    using var pet := (if preferDog then new Dog() else new Animal());
    Console.WriteLine(pet.Speak());
  end Main;
end Program;
