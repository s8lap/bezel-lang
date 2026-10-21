//! case: s17_5_0017
//! doc: spec
//! section: 17.5
//! title: a nullable Animal left operand over an Array<Animal> is rejected
//! rule: "Where the selected `Contains` declares a **non-nullable** parameter,
//!   a nullable left operand is a **compile error** (`T?` does not implicitly
//!   convert to `T`, [§5.1])" ([§17.5]). The element type here is a class, not
//!   a value type.
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Animal
end Animal;

public class Dog: Animal
end Dog;

public class Program
  public static method Main()
    var a: Array<Animal> := [new Dog()];
    var d: Animal? := null;
    var found := d in a;
    Console.WriteLine(found);
  end Main;
end Program;
