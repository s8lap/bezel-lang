//! case: s7_5_7108
//! doc: spec
//! section: 7.5
//! title: `?.First` / `?.Last` on a nullable LinkedList parameter: head, tail, and a null receiver
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! output: 4
//!   6
//!   -1
//!   -1
module Conf;
public class Program
  public static method Both(m: LinkedList<int>?)
    Console.WriteLine(m?.First ?? -1);
    Console.WriteLine(m?.Last ?? -1);
  end Both;
  public static method Main()
    using var l := new LinkedList<int>();
    _ := l.AddLast(4);
    _ := l.AddLast(6);
    Both(l);
    Both(null);
  end Main;
end Program;
