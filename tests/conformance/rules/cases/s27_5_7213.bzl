//! case: s27_5_7213
//! doc: spec
//! section: 27.5
//! title: a local shared by two closures, one of which escapes, still reads
//!   the other's writes
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa.
//! cites: 27.5, 14.1
//! expect: run
//! stdout: 41000
module Conf;

public class Program
  public static method Two<T>(z: T, i: int, out2: Array<() -> int>): int
    var s := i;
    var inc := lambda() => do
      s := s + 1;
    end;
    var get := lambda() => do
      return s;
    end;
    inc();
    out2.Add(get);
    inc();
    return s;
  end Two;

  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var l := new Array<() -> int>();
      total := total + Program.Two<long>(7L, i, l);
      total := total + l[0]();
    end for;
    Console.WriteLine(total);
  end Main;
end Program;
