//! case: s5_1_0158
//! doc: spec
//! section: 5.1
//! title: byte and sbyte arguments widen to int at DateOnly's positional parameters
//! rule: | `byte` | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` | ... | `sbyte` | `short`, `int`, `long`, `float`, `double`, `decimal` |
//! cites: 5.1
//! expect: run
//! output: 255
//!   12
//!   31
module Conf;

public class Program
  public static method Main()
    var y: byte := 255;
    var m: sbyte := 12;
    var d: byte := 31;
    var x := new DateOnly(y, m, d);
    Console.WriteLine(x.Year);
    Console.WriteLine(x.Month);
    Console.WriteLine(x.Day);
  end Main;
end Program;
