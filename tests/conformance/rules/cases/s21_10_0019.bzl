//! case: s21_10_0019
//! doc: spec
//! section: 21.10
//! title: a local record overrides ToString and compares by value like a module-level record
//! rule: "are all computed by their existing rules from the declaration."
//! cites: 8, 9
//! expect: run
//! output: P<7>
//!   P<8>
//!   True
module Conf;

public class Program
  public static method Main()
    record P(A: int)
      public override method ToString(): string
        return $"P<{A}>";
      end ToString;
    end P;
    Console.WriteLine(new P(7).ToString());
    Console.WriteLine($"{new P(8)}");
    Console.WriteLine(new P(1) = new P(1));
  end Main;
end Program;
