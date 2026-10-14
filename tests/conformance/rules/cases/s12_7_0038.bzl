//! case: s12_7_0038
//! doc: spec
//! section: 12.7
//! title: a type-narrowed carrier that is not reassigned keeps the narrowing
//! rule: When the test succeeds and the tested expression is one of §7.4's narrowing carriers, the carrier is **flow-narrowed** to `T` within the controlled block (v0.338 §12.7 line 2106)
//! cites: 7.4, 17.4
//! expect: run
//! output: woof
module Conf;

public class Animal
  public constructor() end constructor;
  public method Speak(): string => "animal";
end Animal;

public class Dog: Animal
  public constructor() end constructor;
  public method Bark(): string => "woof";
end Dog;

public class Program
  public static method Main()
    var a: Animal := new Dog();
    if a is Dog then
      Console.WriteLine(a.Bark());
    end if;
  end Main;
end Program;
