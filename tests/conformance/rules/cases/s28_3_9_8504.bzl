//! case: s28_3_9_8504
//! doc: spec
//! section: 28.3.9
//! title: a closure last called before the move of its owner is admitted, nested iterator
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use of
//!   it
//! cites: 28.3.9, 28.3
//! expect: run
//! stdout: 8
//! notes: Pins #9078 (admitting twin of s28_3_9_8503). f() = 7 before the move, Take = 1, the
//!   inner iterator yields 8 and the outer one passes it on: 8.
module Conf;
public class Res
  public N: int := 7;
  public constructor() end constructor;
end Res;
public class Sink
  public static method Take(owned r: Res): int => 1;
end Sink;

public class Program
  public static method Inner(): IEnumerable<int>
    var a := new Res();
    var f := lambda(): int => a.N;
    var x := f();
    var t := Sink.Take(move a);
    yield return x + t;
  end Inner;
  public static method Gen(): IEnumerable<int>
    for var v in Inner() do yield return v; end;
  end Gen;
  public static method Main(): int
    var r := 0;
    for var x in Gen() do r := r + x; end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;
