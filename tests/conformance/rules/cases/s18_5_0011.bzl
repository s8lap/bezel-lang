//! case: s18_5_0011
//! doc: spec
//! section: 18.5
//! title: a present nullable hole with a format spec dispatches ordinarily
//! rule: Continuing s18_5_0010's worked example: "`x := 3.0; Console.WriteLine($
//!   \"[{x,8:F2}]\"); // prints: [    3.00] — present value: ordinary dispatch`."
//! cites: 18.5
//! expect: run
//! stdout: [    3.00]
module Conf;

public class Program
  public static method Main()
    var x: double? := 3.0;
    Console.WriteLine($"[{x,8:F2}]");
  end Main;
end Program;
