//! case: s28_9_4504
//! doc: spec
//! section: 28.9
//! title: a function-typed local aliasing another survives an await and is called
//!   after resuming
//! rule: Function-typed fields, parameters, and locals are not subject to `owned`
//!   annotations and carry no disposal obligation
//! cites: 28.3, 30.1
//! expect: run
//! output: 4
//!   2
//!   4
//!   DONE
//! issue: 8440
module Conf;

public class C
  public static async method Run(i: int): Task<int>
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    var cmp: (int, int) -> int := g;
    if i mod 2 = 0 then
      cmp := lambda(x: int, y: int) => y - x;
    end if;
    await Task.Delay(0);
    return cmp(1, 2) + g(4, 1);
  end Run;
  public static async method Main(): Task<int>
    for var i in 0..<3 do
      Console.WriteLine(await Run(i));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
