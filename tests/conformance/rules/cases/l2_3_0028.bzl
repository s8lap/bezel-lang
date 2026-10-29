//! case: l2_3_0028
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString(int) and ToString(int, 16) assigned to locals in a loop
//! rule: Convert.ToString(value: int) returns the text the int renders as under its own ToString(); ToString(value, toBase) returns the text value.ToString(toBase) returns, so base 16 renders 255 as ff (stdlib 2.3, 9.1).
//! cites: 2.3, 9.1
//! expect: run
//! output: 7
//!   77
//!   7
//!   77
//!   7
//!   77
module Conf;

public class Program
  public static method Main()
    var v := 7;
    for var i in 0..<3 do
      var s := Convert.ToString(v);
      Console.WriteLine(s);
      var t := Convert.ToString(v, 16);
      Console.WriteLine(t + s);
    end;
  end Main;
end Program;
