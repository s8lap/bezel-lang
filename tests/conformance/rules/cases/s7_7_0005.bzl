//! case: s7_7_0005
//! doc: spec
//! section: 7.7
//! title: ??= evaluates its right-hand side and assigns only when the left is null
//! rule: The compound form `??=` evaluates its right-hand side, and invokes
//!   the setter, only when the assignment occurs.
//! cites: 6.6
//! expect: run
//! stdout: cached
module Conf;

public class Program
  public static method Fresh(): string
    Console.WriteLine("fresh made");
    return "F";
  end Fresh;

  public static method Main()
    var cache: string? := "cached";
    cache ??= Fresh();
    Console.WriteLine(cache);
  end Main;
end Program;
