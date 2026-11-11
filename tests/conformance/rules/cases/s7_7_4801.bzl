//! case: s7_7_4801
//! doc: spec
//! section: 7.7
//! title: ??= over a string? local holding a value does not evaluate its right operand
//! rule: The compound form `??=` evaluates its right-hand side, and invokes the setter, only when the assignment occurs
//! cites: 7.7, 6.6
//! expect: run
//! output: a
module Conf;

public class Program
  public static method Tag(): string
    Console.WriteLine("evaluated");
    return "b";
  end Tag;

  public static method Main()
    var s: string? := "a";
    s ??= Tag();
    Console.WriteLine(s);
  end Main;
end Program;
