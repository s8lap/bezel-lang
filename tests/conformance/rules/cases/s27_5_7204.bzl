//! case: s27_5_7204
//! doc: spec
//! section: 27.5
//! title: a closure argument forwarded through a recursive generic callee
//!   stays callable afterwards
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! output: 600
//!   8400
module Conf;

public class Program
  public static method Rec<U>(l: Array<U>, x: U, n: int): int
    var k: (int) -> int := lambda(z: int) => z + 1;
    if n <= 0 then
      l.Add(x);
      return k(0);
    end if;
    return Program.Rec<U>(l, x, n - 1) + 1;
  end Rec;

  public static method Main()
    var keep := new Array<() -> int>();
    var total := 0;
    for var i in 0..<200 do
      total := total + Program.Rec<() -> int>(keep, lambda() => 42, 2);
    end for;
    var s := 0;
    for var g in keep do
      s := s + g();
    end for;
    Console.WriteLine(total);
    Console.WriteLine(s);
  end Main;
end Program;
