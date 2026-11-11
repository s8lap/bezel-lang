//! case: s28_9_4503
//! doc: spec
//! section: 28.9
//! title: two aliases of one function value reassigned independently each call their
//!   own current value
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.3
//! expect: run
//! output: 7
//!   1
//!   3
//!   5
//!   DONE
//! issue: 8440
module Conf;

public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<4 do
      var a: (int, int) -> int := g;
      var b: (int, int) -> int := a;
      if i mod 2 = 0 then
        a := lambda(x: int, y: int) => y - x;
      end if;
      if i mod 3 = 0 then
        b := lambda(x: int, y: int) => x + y;
      end if;
      Console.WriteLine(a(1, 2) + b(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
