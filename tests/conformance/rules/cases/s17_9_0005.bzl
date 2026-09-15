//! case: s17_9_0005
//! doc: spec
//! section: 17.9
//! title: nameof of an arithmetic expression is a compile error
//! rule: "Method calls, indexings, arithmetic, and literals are compile
//!   errors: `nameof(Foo())`, `nameof(a + b)`, `nameof(arr[0])`,
//!   `nameof(42)`."
//! cites: 17.9
//! expect: reject
//! diagnostic: nameof
module Conf;

public class Program
  public static method Main()
    var a := 1;
    var b := 2;
    Console.WriteLine(nameof(a + b));
  end Main;
end Program;
