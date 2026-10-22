//! case: s7_11_0016
//! doc: spec
//! section: 7.11
//! title: a user-defined binary operator applies to a nullable record right operand narrowed non-null
//! rule: The rule reads the operand's static type, so an operand that §7.4 has
//!   narrowed non-null is a non-nullable operand and the ordinary operator
//!   applies to it unchanged; no separate rule is needed for that case.
//! cites: 7.11, 7.4, 25.2
//! expect: run
//! output: 4
//!   1
module Conf;

public record Pt(X: int, Y: int)
  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);
end Pt;

public static class Util
  public static method Add(a: Pt, p: Pt?): int
    if p <> null then
      var s := a + p;
      return s.X;
    end if;
    return a.X;
  end Add;
end Util;

public class Program
  public static method Main()
    Console.WriteLine(Util.Add(new Pt(1, 1), new Pt(3, 2)));
    Console.WriteLine(Util.Add(new Pt(1, 1), null));
  end Main;
end Program;
