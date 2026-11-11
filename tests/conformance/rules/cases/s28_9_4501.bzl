//! case: s28_9_4501
//! doc: spec
//! section: 28.9
//! title: a function-typed local aliasing another and reassigned on some paths calls
//!   what it holds
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.3
//! expect: run
//! output: 101
//!   2
//!   4
//!   99
//!   DONE
//! issue: 8440
module Conf;

public class C
  public static method Run(i: int): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    if i mod 2 = 0 then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    if i mod 3 = 0 then
      return cmp(1, 2) + 100;
    end if;
    return cmp(1, 2) + g(4, 1);
  end Run;
  public static method Main(): int
    for var i in 0..<4 do
      Console.WriteLine(Run(i));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
