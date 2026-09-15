//! case: s17_6_0011
//! doc: spec
//! section: 17.6
//! title: ++ on a record that declares operator + is rejected
//! rule: "On an operand of any other type the statement is a compile error on
//!   a separate ground: `++`/`--` are not user-overloadable (§25.1), so a type
//!   spells advancement as `x := x + 1` under its own `operator +`, or as a
//!   named method."
//! cites: 17.6, 25.1, 6.6
//! expect: reject
//! diagnostic: not user-overloadable
module Conf;

public record Ticks(Value: int)
  public static method operator +(t: Ticks, n: int): Ticks =>
    new Ticks(t.Value + n);
end Ticks;

public class Program
  public static method Main()
    var t := new Ticks(1);
    t++;
  end Main;
end Program;
