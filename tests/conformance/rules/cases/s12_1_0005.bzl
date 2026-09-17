//! case: s12_1_0005
//! doc: spec
//! section: 12.1
//! title: a virtual override runs through a base-typed reference
//! rule: A method, property, or indexer is non-virtual unless marked
//!   virtual. To override a virtual member in a derived class, mark the
//!   matching member override.
//! cites: 12.1
//! expect: run
//! stdout: Woof
module Conf;

public class Animal
  public constructor()
  end constructor;

  public virtual method Speak(): string => "generic sound";
end Animal;

public class Dog: Animal
  public constructor()
  end constructor;

  public override method Speak(): string => "Woof";
end Dog;

public class Program
  public static method PrintSpeak(a: Animal)
    Console.WriteLine(a.Speak());
  end PrintSpeak;

  public static method Main()
    using var d := new Dog();
    PrintSpeak(d);
  end Main;
end Program;
