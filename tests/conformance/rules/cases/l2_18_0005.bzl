//! case: l2_18_0005
//! doc: stdlib
//! section: 2.18
//! title: floating P scales by 100 and appends a percent suffix
//! rule: Floating admits P/Pn percent (×100, % suffix).
//! cites: 2.18
//! expect: run
//! stdout: 50.0%
module Conf;

public class Program
  public static method Main()
    var x := 0.5;
    Console.WriteLine($"{x:P1}");
  end Main;
end Program;
