//! case: s7_4_0050
//! doc: spec
//! section: 7.4
//! title: a declaration whose initializer is a new class instance narrows a nullable local for an argument
//! rule: "from a declaration whose initializer is such an `expr`" ([§7.4]), and "where `expr`'s static type is non-nullable": the `new Dog()` initializer has a non-nullable static type, so the local is narrowed and passes to a non-nullable parameter.
//! cites: 7.4
//! expect: run
//! stdout: 3
//! issue: 6223
module Conf;

public class Dog
  public method Speak(): int => 3;
end Dog;

public static class S
  public static method D(x: Dog): int => x.Speak();
end S;

public class Program
  public static method Main()
    var d: Dog? := new Dog();
    Console.WriteLine(S.D(d));
  end Main;
end Program;
