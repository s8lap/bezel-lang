//! case: s7_7_4703
//! doc: spec
//! section: 7.7
//! title: Dog? ??= Animal is rejected because a base class value does not convert down to the subclass
//! rule: for a left operand of type `T?`, the right operand must be implicitly convertible
//! cites: 6.6, 5.1
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Animal
  public constructor() end constructor;
end Animal;

public class Dog: Animal
  public constructor() end constructor;
end Dog;

public class Program
  public static method Main()
    var d: Dog? := null;
    d ??= new Animal();
    Console.WriteLine(d is null);
  end Main;
end Program;
