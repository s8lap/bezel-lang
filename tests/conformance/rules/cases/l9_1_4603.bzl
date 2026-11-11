//! case: l9_1_4603
//! doc: stdlib
//! section: 9.1
//! title: int.ParseOrNull is null for padded, prefixed, separated and fractional input
//! rule: an optional leading `+` or `-`, then one or more digits of that radix, and nothing else
//! cites: 9.1, 1
//! expect: run
//! stdout: null null null null null
module Conf;

public class Program
  public static method Main()
    var a := int.ParseOrNull(" 42");
    var b := int.ParseOrNull("42 ");
    var c := int.ParseOrNull("0x2A");
    var d := int.ParseOrNull("1_000");
    var e := int.ParseOrNull("4.0");
    Console.WriteLine($"{a} {b} {c} {d} {e}");
  end Main;
end Program;
