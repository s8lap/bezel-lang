//! case: l9_3_4601
//! doc: stdlib
//! section: 9.3
//! title: decimal.ParseOrNull parses 007.50 and is null for point-less, exponent and separator spellings
//! rule: `"5"`, `"+5"`, `"-0.25"` and `"007.50"` are admitted; `".5"`, `"5."`, `"."`, `"1.2.3"`, `"1e3"` and `"1_000"` raise `FormatException`
//! cites: 9.3
//! expect: run
//! stdout: 7.5 null null null null null
module Conf;

public class Program
  public static method Main()
    var a := decimal.ParseOrNull("007.50");
    var b := decimal.ParseOrNull(".5");
    var c := decimal.ParseOrNull("5.");
    var d := decimal.ParseOrNull("1e3");
    var e := decimal.ParseOrNull("1_000");
    var f := decimal.ParseOrNull(" 5");
    Console.WriteLine($"{a} {b} {c} {d} {e} {f}");
  end Main;
end Program;
