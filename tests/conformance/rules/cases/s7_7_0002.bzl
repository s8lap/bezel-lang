//! case: s7_7_0002
//! doc: spec
//! section: 7.7
//! title: the right operand of ?? runs only when the left operand is null
//! rule: The left operand is evaluated exactly once, and the right operand
//!   is evaluated only when the left operand's value is `null` — when the
//!   left is non-null the right operand expression does not run at all.
//! expect: run
//! output: Alice
module Conf;

public class Program
  public static method Fallback(): string
    Console.WriteLine("fallback ran");
    return "F";
  end Fallback;

  public static method Main()
    var name: string? := "Alice";
    var display := name ?? Fallback();
    Console.WriteLine(display);
  end Main;
end Program;
