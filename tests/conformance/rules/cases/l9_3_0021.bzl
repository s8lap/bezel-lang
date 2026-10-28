//! case: l9_3_0021
//! doc: stdlib
//! section: 9.3
//! title: decimal.TryParse returns false exactly where Parse raises
//! rule: stdlib §9.3 (v0.345): "`TryParse` returns `false` in every case where `Parse` would
//!   raise" — and the out slot keeps the default value.
//! cites: 9.3
//! expect: run
//! output: False\n0
module Conf;

public class Program
  public static method Main()
    var v: decimal := 0m;
    var ok := decimal.TryParse("nope", out v);
    Console.WriteLine(ok);
    Console.WriteLine(v);
  end Main;
end Program;
