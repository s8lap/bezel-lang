//! case: l9_1_0015
//! doc: stdlib
//! section: 9.1
//! title: TryParse(s, radix, out) is shape (c) where Parse raises
//! rule: stdlib §9.1 declares `TryParse(s: string, radix: int, out result: int):
//!     bool` — "shape (c): false (out = 0) exactly where Parse(s, radix) raises
//!     FormatException/OverflowException". `zz` is not a radix-16 digit string,
//!     so `Parse(s, radix)` raises `FormatException` and the `Try` form returns
//!     false with the out slot at 0.
//! cites: 9.1, 1
//! expect: run
//! output: False
//!   0
module Conf;

public class Program
  public static method Main()
    var ok := int.TryParse("zz", 16, out var v);
    Console.WriteLine(ok);
    Console.WriteLine(v);
  end Main;
end Program;
