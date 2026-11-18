//! case: s28_9_6804
//! doc: spec
//! section: 28.9
//! title: a function assigned from `n ?? g2` in an inner scope to an outer local and stored stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 12.7, 7.7
//! expect: run
//! stdout: 1400
//! notes: k >= 0 holds on every iteration, so b is g2 and 1400 = 200 iterations of g2(5, 2) = 5 + 2 = 7.
module Conf;

type Fn := (int, int) -> int;

public class Program
  public static method Fill(fs: Array<(int, int) -> int>, os: Array<Object>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k - k;
    var n: ((int, int) -> int)? := null;
    var b: ((int, int) -> int)? := null;
    if k >= 0 then
      var g2: (int, int) -> int := lambda(x: int, y: int) => x + y + k - k;
      b := n ?? g2;
    end;
    fs.Add(b!);
  end Fill;

  public static method Main()
    var t := 0;
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      var os: Array<Object> := new Array<Object>();
      Fill(fs, os, i);
      for var f in fs do t := t + f(5, 2); end for;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
