//! case: s27_5_7218
//! doc: spec
//! section: 27.5
//! title: a closure added to a collection stays callable after its local is
//!   reassigned inside nested loops
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 40200
module Conf;

type Fn := (int, int) -> int;

public class Program
  public static method Fill(fs: Array<Fn>, k: int)
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    fs.Add(b);
    for var i in 0..<2 do
      for var j in 0..<2 do
        b := g;
        b := lambda(x: int, y: int) => 9;
      end for;
    end for;
  end Fill;

  public static method Main()
    var t := 0;
    for var k in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      Program.Fill(fs, k);
      t := t + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
