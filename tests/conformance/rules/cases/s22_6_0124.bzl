//! case: s22_6_0124
//! doc: spec
//! section: 22.6
//! title: a narrowed byte? is read as byte and widens to int at named and positional constructor parameters
//! rule: so a narrowed `d: Dog?` is exact at a `Dog` parameter and converts to an `Animal` or a `Dog?` one ... | `byte` | `short`, `ushort`, `int`, `uint`, `long`, `ulong`, `float`, `double`, `decimal` |
//! cites: 22.6, 5.1
//! expect: run
//! output: 400
//!   200
module Conf;

public class Program
  public static method Pick(v: int): byte?
    return byte(v);
  end Pick;

  public static method Main()
    var m := Pick(200);
    if m <> null then
      var x := new TimeOnly(1, 2, millisecond := m);
      var y := new DateTime(2020, 1, 1, 1, 1, 1, millisecond := m);
      Console.WriteLine(x.Millisecond + y.Millisecond);
      var w := new DateTime(2020, 1, 1, 1, 1, 1, m);
      Console.WriteLine(w.Millisecond);
    end if;
  end Main;
end Program;
