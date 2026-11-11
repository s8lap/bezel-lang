//! case: l9_2_4601
//! doc: stdlib
//! section: 9.2
//! title: double.ParseOrNull reads back the three non-finite renderings
//! rule: `Parse` reads them back as `NaN`, `PositiveInfinity` and `NegativeInfinity`
//! cites: 9.2, 2.18
//! expect: run
//! stdout: NaN Infinity -Infinity
module Conf;

public class Program
  public static method Main()
    var a := double.ParseOrNull("NaN");
    var b := double.ParseOrNull("Infinity");
    var c := double.ParseOrNull("-Infinity");
    Console.WriteLine($"{a} {b} {c}");
  end Main;
end Program;
