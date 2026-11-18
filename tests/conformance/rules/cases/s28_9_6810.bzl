//! case: s28_9_6810
//! doc: spec
//! section: 28.9
//! title: a nullable function local assigned `n ?? g` then `n ?? h` stores the second value, callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 12.7, 7.7
//! expect: run
//! stdout: 2000
//! notes: The last assignment wins, so b is h and 2000 = 200 iterations of h(5, 2) = 5 * 2 = 10.
module Conf;

type Fn := (int, int) -> int;

public class Program
  public static method Fill(fs: Array<(int, int) -> int>, os: Array<Object>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k - k;
    var n: ((int, int) -> int)? := null;
    var h: (int, int) -> int := lambda(x: int, y: int) => x * y + k - k;
    var b: ((int, int) -> int)? := null;
    b := n ?? g;
    b := n ?? h;
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
