//! case: s7_4_0155
//! doc: spec
//! section: 7.4
//! title: both operands of an and-conjunction are narrowed for Math.Min and Math.Clamp
//! rule: in `A and B`, `B` (and the true branch) is checked under `A`'s true-facts
//! cites: 2.2, 7.4
//! expect: run
//! output: 3
//!   3
//! issue: 7660
module Conf;

public class Program
  public static method IdI(x: int?): int? return x; end IdI;

  public static method Main()
    var d: int? := IdI(3);
    var e: int? := IdI(4);
    if d <> null and e <> null then
      Console.WriteLine(Math.Min(d, e));
      Console.WriteLine(Math.Clamp(d, 0, e));
    end if;
  end Main;
end Program;
