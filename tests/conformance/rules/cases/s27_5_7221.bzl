//! case: s27_5_7221
//! doc: spec
//! section: 27.5
//! title: a closure stored as a dictionary value stays callable after its
//!   source local is reassigned
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 40200
module Conf;

type Fn := (int, int) -> int;

public class Program
  public static method Fill(d: Dictionary<int, Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    d.Add(1, b);
    b := g;
  end Fill;

  public static method Main()
    var t := 0;
    for var k in 0..<200 do
      var d: Dictionary<int, Fn> := new Dictionary<int, Fn>();
      Program.Fill(d, k);
      t := t + d[1](1, 2) + d[1](4, 1);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
