//! case: l5_1_0005
//! doc: stdlib
//! section: 5.1
//! title: Min and Max without a selector are admitted over an Array<DateTime>
//! rule: `Min` and `Max` without a selector are generic over comparable types:
//! cites: 5.1, 2.5, 14.2
//! expect: run
//! stdout: admitted
module Conf;

public class Program
  public static method Main()
    var arr: Array<DateTime> := [new DateTime(2021, 1, 1), new DateTime(2019, 1, 1)];
    _ := arr.Min();
    _ := arr.Max();
    Console.WriteLine("admitted");
  end Main;
end Program;
