//! case: s25_2_1210
//! doc: spec
//! section: 25.2
//! title:int mod a TimeSpan has no applicable operator
//! rule:applies is a compile error at the expression
//! cites: 25.1, 2.5
//! expect: reject
//! diagnostic: operator
module Conf;

public class Program
  public static method Main()
    var a := TimeSpan.FromTicks(5L);
    var r := 10 mod a;
    Console.WriteLine(r.Ticks);
  end Main;
end Program;
