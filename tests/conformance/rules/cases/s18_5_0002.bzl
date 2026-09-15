//! case: s18_5_0002
//! doc: spec
//! section: 18.5
//! title: a format spec on a double dispatches to ToString(format) with exact fixed-point rendering
//! rule: "`$\"{amount:F2}\"`           // \"1234.50\""
//! cites: 18.5
//! expect: run
//! stdout: 1234.50
module Conf;

public class Program
  public static method Main()
    var amount: double := 1234.5;
    Console.WriteLine($"{amount:F2}");
  end Main;
end Program;
