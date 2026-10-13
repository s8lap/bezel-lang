//! case: s12_7_0005
//! doc: spec
//! section: 12.7
//! title: an is test against a function type tests the value's formed type, not a retyped view
//! rule: A test whose target is a function type (§27.1) — `o is (Animal) ->
//!   ()`, `o as ((int) -> int)` — is decided by **identity of the function
//!   type**, not by §27.1.1's assignability ... over `f: (Animal) -> ()`,
//!   after `var g: (Dog) -> () := f; var o: Object := g;`, `o is (Animal) ->
//!   ()` is `true` and `o is (Dog) -> ()` is `false`.
//! cites: 12.7, 27.1.1
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
    Console.WriteLine(o is (Animal) -> ());
    Console.WriteLine(o is (Dog) -> ());
  end Main;
end Program;
