//! case: s28_9_7119
//! doc: spec
//! section: 28.9
//! title: a closure local assigned through `b := n ?? g;` (n null) into a kept store stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 7.7
//! expect: run
//! stdout: 40200
module Conf;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var n: ((int, int) -> int)? := null;
    var b: (int, int) -> int := lambda(x: int, y: int) => 0;
    b := n ?? g;
    fs.Add(b);
  end Fill;
  public static method Main(): int
    var t := 0;
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      t := t + fs[0](1, 2) + fs[0](4, 1);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end C;
