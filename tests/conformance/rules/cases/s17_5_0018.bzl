//! case: s17_5_0018
//! doc: spec
//! section: 17.5
//! title: a null Animal left operand is admitted where the element parameter is nullable
//! rule: "Where the selected `Contains` parameter is itself nullable — ... a
//!   nullable left operand is **admitted**: the test performed is the
//!   null-safe equality of [§8.4]" ([§17.5]).
//! cites: 17.5, 8.4
//! expect: run
//! stdout: True
module Conf;

public class Animal
end Animal;

public class Dog: Animal
end Dog;

public class Program
  public static method Main()
    var a: Array<Animal?> := [new Dog(), null];
    var d: Animal? := null;
    var found := d in a;
    Console.WriteLine(found);
  end Main;
end Program;
