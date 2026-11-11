//! case: l9_1_4613
//! doc: stdlib
//! section: 9.1
//! title: long.TryParse is false with out 0 one past the maximum, and ulong likewise
//! rule: shape (c): false (out = 0)
//! cites: 9.1, 1
//! expect: run
//! stdout: False 0 False 0
module Conf;

public class Program
  public static method Main()
    var a: long;
    var b: ulong;
    var okA := long.TryParse("9223372036854775808", out a);
    var okB := ulong.TryParse("18446744073709551616", out b);
    Console.WriteLine($"{okA} {a} {okB} {b}");
  end Main;
end Program;
