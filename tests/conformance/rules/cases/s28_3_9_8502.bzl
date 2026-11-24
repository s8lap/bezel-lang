//! case: s28_3_9_8502
//! doc: spec
//! section: 28.3.9
//! title: a closure last called before the move of its owner is admitted in an iterator
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use of
//!   it
//! cites: 28.3.9, 28.3
//! expect: run
//! stdout: 8
//! notes: Pins #9078 (admitting twin of s28_3_9_8501). `f()` is evaluated before `move a`, so no
//!   later use of the borrow follows the consume site; the yield after the move reads only
//!   the ints `x` and `t`. f() = 7, Take = 1, yielded value 8, one iteration: 8.
module Conf;
public class Res
  public N: int := 7;
  public constructor() end constructor;
end Res;
public class Sink
  public static method Take(owned r: Res): int => 1;
end Sink;

public class Program
  public static method Gen(): IEnumerable<int>
    var a := new Res();
    var f := lambda(): int => a.N;
    var x := f();
    var t := Sink.Take(move a);
    yield return x + t;
  end Gen;
  public static method Main(): int
    var r := 0;
    for var v in Gen() do r := r + v; end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
