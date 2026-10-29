//! case: s7_4_0123
//! doc: spec
//! section: 7.4
//! title: A nullable passed to Math.Max after a join point is not narrowed
//! rule: A fact "holds at a point only when it holds on every path that reaches it" (§7.4, v0.350). After `if e <> null then ... end;` the path where the test failed also reaches the join, so `e` is `double?` and Math.Max(e, 1.0) is refused.
//! cites: 7.4, 2.2
//! expect: reject
//! diagnostic: `Math.Max`
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var e: double? := IdD(2.5);
    if e <> null then Console.WriteLine(1); end;
    Console.WriteLine(Math.Max(e, 1.0));
  end Main;
end Program;
