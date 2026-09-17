//! case: s12_3_0001
//! doc: spec
//! section: 12.3
//! title: base.Member bypasses virtual dispatch
//! rule: Inside a derived member, base.Name(...) calls the base class's
//!   implementation, bypassing virtual dispatch.
//! cites: 12.3
//! expect: run
//! stdout: generic sound, but really Woof
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(): string => "generic sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public override method Speak(): string => $"{base.Speak()}, but really Woof";
end Dog;

public class Program
  public static method Main()
    using var d := new Dog();
    Console.WriteLine(d.Speak());
  end Main;
end Program;
