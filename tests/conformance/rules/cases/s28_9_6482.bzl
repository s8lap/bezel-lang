//! case: s28_9_6482
//! doc: spec
//! section: 28.9
//! title: an alias added to the caller's array by a helper stays callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 28.9, 27.5
//! expect: run
//! stdout: 40200
module Conf;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    fs.Add(a);
  end Fill;
  public static method Main(): int
    var total := 0;
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      total := total + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(total);
    return 0;
  end Main;
end C;
