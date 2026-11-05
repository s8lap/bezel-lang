//! case: s25_2_1203
//! doc: spec
//! section: 25.2
//! title:a float left operand and a TimeSpan right operand has no operator
//! rule:Parameters match operands in order — there is no automatic commutation
//! cites: 2.5
//! expect: reject
//! diagnostic: operator
module Conf;

public class Program
  public static method Main()
    var a := TimeSpan.FromTicks(5L);
    var r := 2.0f * a;
    Console.WriteLine(r.Ticks);
  end Main;
end Program;
