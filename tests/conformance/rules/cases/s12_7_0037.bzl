//! case: s12_7_0037
//! doc: spec
//! section: 12.7
//! title: reassigning a type-narrowed carrier drops the narrowing whatever the right-hand side
//! rule: Reassignment within the controlled block invalidates the type narrowing, whatever the right-hand side's type (the assignment may still establish §7.4's non-null fact) (v0.338 §12.7 line 2114)
//! cites: 7.4, 17.4
//! expect: reject
//! diagnostic: no member
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
      a := new Animal();
      Console.WriteLine(a.Bark());
    end if;
  end Main;
end Program;
