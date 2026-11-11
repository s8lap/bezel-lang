//! case: s28_9_4505
//! doc: spec
//! section: 28.9
//! title: a function-typed local reassigned inside a loop holds the last assigned
//!   value after the loop
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.3
//! expect: run
//! output: 3
//!   3
//!   3
//!   3
//!   -1
//!   DONE
//! issue: 8440
module Conf;

public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    for var i in 0..<4 do
      if i mod 2 = 0 then
        cmp := lambda(x: int, y: int) => y - x;
      else
        cmp := g;
      end if;
      Console.WriteLine(g(4, 1));
    end for;
    Console.WriteLine(cmp(1, 2));
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
