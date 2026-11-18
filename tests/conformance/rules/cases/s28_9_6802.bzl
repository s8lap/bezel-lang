//! case: s28_9_6802
//! doc: spec
//! section: 28.9
//! title: a function value stored via `o as Fn` after an `as Dog` test of the same local stays callable
//! rule: the type's lifetime is managed by the runtime without user involvement
//! cites: 28.9, 12.7, 7.7
//! expect: run
//! stdout: 600
//! notes: Red on a pre-fix bezelc (the chain-hole 782c9c9e2, the #8743 commit, closes: a crash there); 600 = 200 iterations of g(5, 2) = 3.
module Conf;

type Fn := (int, int) -> int;

public class Dog
  public N: int;
  public constructor() N := 1; end constructor;
end Dog;

public class Program
  public static method Fill(fs: Array<(int, int) -> int>, os: Array<Object>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k - k;
    var n: ((int, int) -> int)? := null;
    var o: Object := n ?? g;
    var d := o as Dog;
    var q := o as Fn;
    if q <> null then fs.Add(q!); end;
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
