//! case: s7_7_4802
//! doc: spec
//! section: 7.7
//! title: ?? over a parameter narrowed non-null by a <> null test is admitted and selects the left operand
//! rule: `a ?? b` returns `a` if non-null, else `b`.
//! cites: 7.7, 7.4
//! expect: run
//! output: q
//!   n
module Conf;

public class Program
  public static method F(s: string?): string
    if s <> null then
      return s ?? "x";
    end if;
    return "n";
  end F;

  public static method Main()
    Console.WriteLine(F("q"));
    Console.WriteLine(F(null));
  end Main;
end Program;
