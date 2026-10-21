//! case: s17_5_0013
//! doc: spec
//! section: 17.5
//! title: an Animal left operand does not implicitly convert to a Dog element
//! rule: The implicit reference conversions include "**class to base class**,
//!   transitively, up to `Object`" ([§5.1]); the reverse, base to derived, is
//!   not among them, so the left operand is not admissible as
//!   `Contains(item: Dog)`'s argument ([§17.5]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public class Animal
end Animal;

public class Dog: Animal
end Dog;

public class Program
  public static method Main()
    var a: Array<Dog> := [new Dog()];
    var b: Animal := new Dog();
    var found := b in a;
    Console.WriteLine(found);
  end Main;
end Program;
