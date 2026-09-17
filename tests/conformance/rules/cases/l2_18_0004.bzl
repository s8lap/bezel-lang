//! case: l2_18_0004
//! doc: stdlib
//! section: 2.18
//! title: floating F with no digit count defaults to 2 decimals
//! rule: Floating admits F/Fn fixed-point, n decimals, default 2.
//! cites: 2.18
//! expect: run
//! stdout: 3.14
module Conf;

public class Program
  public static method Main()
    var x := 3.14159;
    Console.WriteLine($"{x:F}");
  end Main;
end Program;
