//! case: l9_2_4603
//! doc: stdlib
//! section: 9.2
//! title: double.ParseOrNull is null for padded, separated, hex, bare-point and point-less spellings
//! rule: so `" 5"`, `"5 "`, `"1_000"`, `"0x1p3"`, `".5"` and `"5."` raise `FormatException`
//! cites: 9.2
//! expect: run
//! stdout: null null null null null null
module Conf;

public class Program
  public static method Main()
    var a := double.ParseOrNull(" 5");
    var b := double.ParseOrNull("5 ");
    var c := double.ParseOrNull("1_000");
    var d := double.ParseOrNull("0x1p3");
    var e := double.ParseOrNull(".5");
    var f := double.ParseOrNull("5.");
    Console.WriteLine($"{a} {b} {c} {d} {e} {f}");
  end Main;
end Program;
