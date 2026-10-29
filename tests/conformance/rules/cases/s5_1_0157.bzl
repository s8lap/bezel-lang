//! case: s5_1_0157
//! doc: spec
//! section: 5.1
//! title: a byte argument widens to int at a positional TimeOnly parameter without sign extension
//! rule: | `byte` | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//! cites: 5.1
//! expect: run
//! stdout: 200
module Conf;

public class Program
  public static method Main()
    var m: byte := 200;
    var x := new TimeOnly(1, 2, 3, m);
    Console.WriteLine(x.Millisecond);
  end Main;
end Program;
