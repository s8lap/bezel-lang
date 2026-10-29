//! case: l2_3_0029
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString with base 2 and no base inside one interpolated string
//! rule: Convert.ToString(value: int) returns the text the int renders as under its own ToString(); ToString(value, toBase) returns the text value.ToString(toBase) returns, so base 2 renders 99 as 1100011 (stdlib 2.3, 9.1).
//! cites: 2.3, 9.1
//! expect: run
//! output: a99b1100011
//!   a99b1100011
module Conf;

public class Program
  public static method Main()
    var v := 99;
    for var i in 0..<2 do
      var s := $"a{Convert.ToString(v)}b{Convert.ToString(v, 2)}";
      Console.WriteLine(s);
    end;
  end Main;
end Program;
