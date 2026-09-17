//! case: s19_3_0009
//! doc: spec
//! section: 19.3
//! title: an unbound type pattern subtype-matches a reference-typed scrutinee
//! rule: Type patterns without binding — `string`, `int`, `Dog` — subtype
//!   match: a `Dog` value matches an `Animal` pattern. Subtype matching is
//!   a property of reference-typed scrutinees.
//! cites: 19.3
//! expect: run
//! stdout: dog
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
  public static method Describe(a: Animal): string => (case a of
    var d: Dog => "dog",
    _          => "animal");

  public static method Main()
    var d := new Dog();
    Console.WriteLine(Describe(d));
    _ := d;
  end Main;
end Program;
