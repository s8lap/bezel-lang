//! case: s22_6_0084
//! doc: spec
//! section: 22.6
//! title: two interfaces with same-name overloads bind each member to its exact-typed witness
//! rule: parameter types that are exactly equal under the interface's substitution; An exact match is the only relation that distinguishes one applicable candidate from another at an argument position (v0.338 §11.2, §22.6)
//! cites: 11.2, 22.6
//! expect: run
//! output: 112 | 12
module Conf;

public class Animal
  public constructor() end constructor;
end Animal;

public class Dog: Animal
  public constructor() end constructor;
end Dog;

public interface IA
  method F(x: Animal): int;
end IA;

public interface ID
  method F(x: Dog): int;
end ID;

public class C: IA, ID
  public constructor() end constructor;
  public method F(x: Dog): int => 2;
  public method F(x: Animal): int => 1;
end C;

public class Program
  public static method Main()
    using var c := new C();
    var a: IA := c;
    var d: ID := c;
    using var an := new Animal();
    using var g := new Dog();
    Console.WriteLine(a.F(an).ToString() + a.F(g).ToString() + d.F(g).ToString() + " | " + c.F(an).ToString() + c.F(g).ToString());
  end Main;
end Program;
