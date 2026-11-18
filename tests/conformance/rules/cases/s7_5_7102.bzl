//! case: s7_5_7102
//! doc: spec
//! section: 7.5
//! title: `?.First` / `?.Last` over LinkedList<int> and LinkedList<string> in a loop give head and tail
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! output: 3300
//!   3900
//!   6
module Conf;
public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    _ := l.AddLast(11);
    _ := l.AddLast(22);
    var total := 0;
    for var i in 0..<100 do
      total := total + (l?.First ?? 0) + (l?.Last ?? 0);
    end;
    Console.WriteLine(total);
    using var ls := new LinkedList<string>();
    _ := ls.AddLast("hello" + "x");
    for var i in 0..<100 do
      var f := ls?.First ?? "none";
      total := total + f.Length;
    end;
    Console.WriteLine(total);
    Console.WriteLine(ls?.First?.Length ?? -5);
  end Main;
end Program;
