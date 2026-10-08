//! case: s12_7_0001
//! doc: spec
//! section: 12.7
//! title: is T narrows the tested variable to T in the true branch
//! rule: is T tests whether a value is an instance of T and produces bool.
//!   When the test succeeds and the tested expression is a simple variable
//!   other than a ref parameter, the variable is flow-narrowed to T within
//!   the controlled block.
//! cites: 12.7
//! expect: run
//! stdout: Woof
module Conf;

public class Animal
  public constructor()
  end constructor;
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public method Bark(): string => "Woof";
end Dog;

public class Program
  public static method Main()
    using var a: Animal := new Dog();
    if a is Dog then
      Console.WriteLine(a.Bark());
    end if;
  end Main;
end Program;
