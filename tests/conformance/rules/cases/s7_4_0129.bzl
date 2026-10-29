//! case: s7_4_0129
//! doc: spec
//! section: 7.4
//! title: A nullable parameter is not narrowed without a null test, so Math.Abs is refused
//! rule: A parameter is narrowed only by the forms of §7.4 (v0.350); with no test, `x: double?` reaches `Math.Abs(x)` as `double?`, which `Abs(x: double)` refuses.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method P(x: double?): double return Math.Abs(x); end P;
  public static method Main()
    Console.WriteLine(P(1.5));
  end Main;
end Program;
