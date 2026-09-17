//! case: s17_9_0004
//! doc: spec
//! section: 17.9
//! title: nameof of a method call is a compile error
//! rule: "Method calls, indexings, arithmetic, and literals are compile
//!   errors: `nameof(Foo())`, `nameof(a + b)`, `nameof(arr[0])`,
//!   `nameof(42)`."
//! cites: 17.9
//! expect: reject
//! diagnostic: nameof
module Conf;

public class Program
  public static method Foo(): int => 1;

  public static method Main()
    Console.WriteLine(nameof(Foo()));
  end Main;
end Program;
