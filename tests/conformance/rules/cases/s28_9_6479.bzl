//! case: s28_9_6479
//! doc: spec
//! section: 28.9
//! title: an alias cast with `as` and added to the caller's array stays
//!   callable
//! rule: the type's lifetime is managed by the runtime without user
//!   involvement
//! cites: 17.4, 28.9
//! expect: run
//! stdout: 40200
module Conf;
type Fn := (int, int) -> int;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    fs.Add((a as Fn)!);
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
