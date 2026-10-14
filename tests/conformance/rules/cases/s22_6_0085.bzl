//! case: s22_6_0085
//! doc: spec
//! section: 22.6
//! title: an interface member binds the inherited override with the exact parameter type
//! rule: parameter types that are exactly equal under the interface's substitution; An exact match is the only relation that distinguishes one applicable candidate from another at an argument position (v0.338 §11.2, §22.6)
//! cites: 11.2, 22.6
//! expect: run
//! output: 200 20 | 200
module Conf;

public class Animal
  public constructor() end constructor;
end Animal;

public class Dog: Animal
  public constructor() end constructor;
end Dog;

public interface I
  method F(x: Dog): int;
end I;

public class B: I
  public constructor() end constructor;
  public virtual method F(x: Animal): int => 10;
  public virtual method F(x: Dog): int => 20;
end B;

public class D: B
  public constructor() end constructor;
  public override method F(x: Dog): int => 200;
  public override method F(x: Animal): int => 100;
end D;

public class Program
  public static method Main()
    using var d := new D();
    var j: I := d;
    using var b := new B();
    var k: I := b;
    using var g := new Dog();
    Console.WriteLine(j.F(g).ToString() + " " + k.F(g).ToString() + " | " + d.F(g).ToString());
  end Main;
end Program;
