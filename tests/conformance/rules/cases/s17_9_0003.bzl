//! case: s17_9_0003
//! doc: spec
//! section: 17.9
//! title: nameof of a type parameter returns the source-level parameter name
//! rule: "For a type parameter `T`, `nameof(T)` returns the source-level name
//!   as written in the declaration: `\"T\"`, `\"TElement\"`, etc. It does not
//!   return the substituted runtime type's name."
//! cites: 17.9
//! expect: run
//! stdout: TElement
module Conf;

public class Box<TElement>
  public constructor()
  end constructor;

  public method Describe(): string => nameof(TElement);
end Box;

public class Program
  public static method Main()
    using var b := new Box<int>();
    Console.WriteLine(b.Describe());
  end Main;
end Program;
