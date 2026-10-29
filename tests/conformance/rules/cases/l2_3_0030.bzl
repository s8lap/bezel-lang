//! case: l2_3_0030
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString results returned from methods and concatenated
//! rule: Convert.ToString(value: int) returns the text the int renders as under its own ToString(); ToString(value, toBase) returns the text value.ToString(toBase) returns, so base 16 renders 255 as ff (stdlib 2.3, 9.1).
//! cites: 2.3, 9.1
//! expect: run
//! output: 000
//!   111
//!   10a10
//!   11b11
module Conf;

public class Program
  public static method F(v: int): string => Convert.ToString(v);
  public static method G(v: int): string
    return Convert.ToString(v, 16);
  end G;
  public static method H(v: int): string
    var s := Convert.ToString(v);
    return s;
  end H;
  public static method Main()
    for var i in 0..<12 do
      if i < 2 or i > 9 then
        Console.WriteLine(Program.F(i) + Program.G(i) + Program.H(i));
      end;
    end;
  end Main;
end Program;
