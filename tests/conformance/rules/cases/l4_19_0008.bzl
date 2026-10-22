//! case: l4_19_0008
//! doc: stdlib
//! section: 4.19
//! title: From at T = string inside a generic body copies a borrowed source like the non-generic call
//! rule: public static method From(items: Array<T>): owned FrozenArray<T>; Copies;
//!   the only construction form for T = string. Construction consumes or copies
//!   its source — after either, no writer can exist.
//! cites: 4.19, 14.5
//! expect: run
//! output: ax zz
//! issue: 6924
module Conf;

public static class Util
  public static method Freeze<T>(a: Array<T>): owned FrozenArray<T>
    return FrozenArray<T>.From(a);
  end Freeze;
end Util;

public class Program
  public static method Main()
    var last := "";
    var i := 0;
    while i < 50 do
      using var a: Array<string> := ["a" + "x", "b" + "y"];
      using var f := Util.Freeze<string>(a);
      a[0] := "zz";
      last := f[0] + " " + a[0];
      i += 1;
    end;
    Console.WriteLine(last);
  end Main;
end Program;
