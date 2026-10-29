//! case: l2_3_0032
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString results through the null-coalescing operator and a null guard
//! rule: Convert.ToString(value: int) returns the text the int renders as under its own ToString(); ToString(value, toBase) returns the text value.ToString(toBase) returns, so base 16 renders 255 as ff (stdlib 2.3, 9.1).
//! cites: 2.3, 9.1, 7.5
//! expect: run
//! output: 5
//!   5
//!   5
//!   5
//!   5
//!   5
module Conf;

public class Program
  public static method Main()
    var v := 5;
    for var i in 0..<2 do
      var n: string? := Convert.ToString(v);
      var t := n ?? "";
      Console.WriteLine(t);
      var u: string? := null;
      var w := u ?? Convert.ToString(v);
      Console.WriteLine(w);
      var x: string? := Convert.ToString(v, 16);
      if x <> null then Console.WriteLine(x); end;
    end;
  end Main;
end Program;
