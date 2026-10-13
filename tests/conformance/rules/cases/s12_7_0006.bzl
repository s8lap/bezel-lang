//! case: s12_7_0006
//! doc: spec
//! section: 12.7
//! title: an as conversion to a function type the value reaches only by variance yields null
//! rule: A test whose target is a function type (§27.1) — `o is (Animal) ->
//!   ()`, `o as ((int) -> int)` — is decided by **identity of the function
//!   type**, not by §27.1.1's assignability ... over `f: (Animal) -> ()`,
//!   after `var g: (Dog) -> () := f; var o: Object := g;`, `o is (Animal) ->
//!   ()` is `true` and `o is (Dog) -> ()` is `false`.
//! cites: 12.7, 17.4, 27.1.1
//! expect: run
//! output: True
//!   False
//! issue: 5826
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;
end Dog;

public class Handlers
  public static method OnAnimal(a: Animal)
  end OnAnimal;
end Handlers;

public class Program
  public static method Main()
    var f: (Animal) -> () := Handlers.OnAnimal;
    var g: (Dog) -> () := f;
    var o: Object := g;
    var asDog := o as ((Dog) -> ());
    var asAnimal := o as ((Animal) -> ());
    Console.WriteLine(asDog = null);
    Console.WriteLine(asAnimal = null);
  end Main;
end Program;
