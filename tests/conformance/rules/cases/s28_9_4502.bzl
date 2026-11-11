//! case: s28_9_4502
//! doc: spec
//! section: 28.9
//! title: a function value stored into a field outlives the local that held it and
//!   still runs
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.3
//! expect: run
//! output: 3
//!   1
//!   3
//!   -1
//!   3
//!   1
//!   DONE
//! issue: 8440
module Conf;

public class H
  public F: (int, int) -> int := lambda(x: int, y: int) => 0;
  public constructor() end constructor;
end H;

public class C
  public static method Fill(h: H, i: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    if i mod 2 = 0 then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    h.F := cmp;
    Console.WriteLine(g(4, 1));
  end Fill;
  public static method Main(): int
    using var h := new H();
    for var i in 0..<3 do
      Fill(h, i);
      Console.WriteLine(h.F(1, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
