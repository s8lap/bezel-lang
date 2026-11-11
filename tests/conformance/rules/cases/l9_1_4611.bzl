//! case: l9_1_4611
//! doc: stdlib
//! section: 9.1
//! title: long.TryParse is true with the value at Int64.MaxValue and Int64.MinValue
//! rule: shape (c): false (out = 0)
//! cites: 9.1, 1
//! expect: run
//! stdout: True 9223372036854775807 True -9223372036854775808
module Conf;

public class Program
  public static method Main()
    var hi: long;
    var lo: long;
    var okHi := long.TryParse("9223372036854775807", out hi);
    var okLo := long.TryParse("-9223372036854775808", out lo);
    Console.WriteLine($"{okHi} {hi} {okLo} {lo}");
  end Main;
end Program;
