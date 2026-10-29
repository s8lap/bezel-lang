//! case: s5_1_0159
//! doc: spec
//! section: 5.1
//! title: byte arguments widen to int at named and positional constructor parameters on every iteration
//! rule: | `byte` | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` | ... Named arguments may appear in any order after positional arguments.
//! cites: 5.1, 22.5
//! expect: run
//! stdout: 40000
module Conf;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0..<100 do
      var b: byte := 200;
      var x := new DateTime(2020, 1, 1, 1, 1, 1, millisecond := b);
      var z := new TimeOnly(1, 2, 3, b);
      t := t + x.Millisecond + z.Millisecond;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
