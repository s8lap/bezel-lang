//! case: l2_3_0031
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString results stored into a string field and reassigned
//! rule: Convert.ToString(value: int) returns the text the int renders as under its own ToString(); ToString(value, toBase) returns the text value.ToString(toBase) returns, so base 16 renders 255 as ff (stdlib 2.3, 9.1).
//! cites: 2.3, 9.1
//! expect: run
//! output: a
//!   11
//!   b
//!   12
module Conf;

public class Box
  public name: string := "";
  public method Set(v: int)
    this.name := Convert.ToString(v);
  end Set;
end Box;

public class Program
  public static method Main()
    for var i in 10..<12 do
      var b := new Box();
      b.name := "x";
      b.Set(i);
      b.name := Convert.ToString(i, 16);
      Console.WriteLine(b.name);
      b.Set(i + 1);
      Console.WriteLine(b.name);
    end;
  end Main;
end Program;
