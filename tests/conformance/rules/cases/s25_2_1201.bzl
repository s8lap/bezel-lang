//! case: s25_2_1201
//! doc: spec
//! section: 25.2
//! title:a double left operand and a TimeSpan right operand has no operator (declared order is
//!   reverse)
//! rule:Parameters match operands in order — there is no automatic commutation
//! cites: 2.5
//! expect: reject
//! diagnostic: operator
module Conf;

public class Program
  public static method F(t: TimeSpan): TimeSpan
    return 3.0 * t;
  end F;

  public static method Main()
    Console.WriteLine(F(TimeSpan.FromTicks(2L)).Ticks);
  end Main;
end Program;
