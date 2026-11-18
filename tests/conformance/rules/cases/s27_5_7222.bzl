//! case: s27_5_7222
//! doc: spec
//! section: 27.5
//! title: a closure stored by an iterator body stays callable after the
//!   iterator reassigns its source local
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9, 29.4
//! expect: run
//! stdout: 40800
module Conf;

type Fn := (int, int) -> int;

public class Program
  public static method Gen(fs: Array<Fn>, k: int): IEnumerable<int>
    var g: Fn := lambda(x: int, y: int) => 0;
    var b: Fn := lambda(x: int, y: int) => x - y + k;
    fs.Add(b);
    yield return 1;
    b := g;
    yield return 2;
  end Gen;

  public static method Main()
    var t := 0;
    for var k in 0..<200 do
      var fs: Array<Fn> := new Array<Fn>();
      var s := 0;
      for var v in Program.Gen(fs, k) do
        s := s + v;
      end for;
      t := t + fs[0](1, 2) + fs[0](4, 1) + s;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
